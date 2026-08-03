/*
 * BalancedVolume — 固化左右声道比例的托盘工具
 * Win32 原生版，无运行库依赖（需要 Windows Vista+ WASAPI）
 *
 * 编译: see Makefile
 */

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <cmath>
#include <string>
#include <algorithm>
#include <vector>

#include "audio.h"

// ============================================================
//  常量与枚举
// ============================================================

static const wchar_t kTrayClass[]  = L"BV_TrayHost";
static const wchar_t kPopupClass[] = L"BV_Popup";

static constexpr UINT WM_TRAYNOTIFY = WM_APP + 1;  // Shell_NotifyIcon 回调
static constexpr UINT WM_SYNC_UI   = WM_APP + 2;   // 音频线程→UI 线程同步
static constexpr UINT TRAY_UID     = 1;

// 弹窗客户区尺寸（与 C# 版 ClientSize 一致）
static constexpr int kPW = 320;
static constexpr int kPH = 200;

// 右键菜单 ID
enum : UINT { IDM_OPEN = 100, IDM_EXIT };

// 三个滑块的索引
enum : int { SLD_MASTER = 0, SLD_LEFT, SLD_RIGHT, SLD_COUNT };

// 滑块控件 ID（连续，方便 GetDlgCtrlID 判断）
enum : int {
    ID_SLD_MASTER = 200,
    ID_SLD_LEFT   = 201,
    ID_SLD_RIGHT  = 202,
};

// ============================================================
//  全局状态
// ============================================================

static HINSTANCE       g_hInst   = nullptr;
static HWND            g_hTray   = nullptr;   // 隐藏宿主窗口（接收托盘消息）
static HWND            g_hPopup  = nullptr;   // 弹出控制面板
static HICON           g_hIcon   = nullptr;
static AudioController* g_audio  = nullptr;

static bool g_syncing = false;  // 防止滑块↔硬件互相触发

static HWND g_sliders[SLD_COUNT];    // master, left, right
static HWND g_valLabels[SLD_COUNT];  // 对应数值文本
static HWND g_hRatioLbl = nullptr;

static HBRUSH g_bgBrush  = nullptr;   // RGB(30,30,30)
static HFONT  g_uiFont   = nullptr;

// ============================================================
//  托盘图标（动态生成，无需外部 .ico 文件）
//  对应 C# 版 BuildTrayIcon() 中的 GDI+ 绘图逻辑
// ============================================================

static HICON CreateSpeakerIcon() {
    constexpr int sz = 16;

    // 使用 BITMAPV5HEADER + 32bpp + alpha 通道，现代 Windows 完整支持
    BITMAPV5HEADER bi  = {};
    bi.bV5Size         = sizeof(bi);
    bi.bV5Width        = sz;
    bi.bV5Height       = -sz;   // top-down
    bi.bV5Planes       = 1;
    bi.bV5BitCount     = 32;
    bi.bV5Compression  = BI_BITFIELDS;
    bi.bV5RedMask      = 0x00FF0000u;
    bi.bV5GreenMask    = 0x0000FF00u;
    bi.bV5BlueMask     = 0x000000FFu;
    bi.bV5AlphaMask    = 0xFF000000u;

    void* pBits = nullptr;
    HDC   dc    = GetDC(nullptr);
    HBITMAP hBmp = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi),
                                     DIB_RGB_COLORS, &pBits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!hBmp) return nullptr;

    // 像素格式：BGRA（内存低字节=B，高字节=A），0xFFFFFFFF = 不透明白色
    auto* p = static_cast<UINT32*>(pBits);
    std::fill(p, p + sz * sz, 0u);  // 全透明

    auto dot = [&](int x, int y) noexcept {
        if (static_cast<unsigned>(x) < static_cast<unsigned>(sz) &&
            static_cast<unsigned>(y) < static_cast<unsigned>(sz))
            p[y * sz + x] = 0xFFFFFFFFu;
    };

    // 扬声器主体矩形 [x=2..5, y=5..10]
    for (int y = 5; y <= 10; ++y)
        for (int x = 2; x <= 5; ++x)
            dot(x, y);

    // 扬声器锥形（梯形，向右展开）
    for (int x = 5; x <= 10; ++x) {
        float t   = static_cast<float>(x - 5) / 5.0f;
        int   top = static_cast<int>(std::roundf(4.0f - t * 3.0f));
        int   bot = static_cast<int>(std::roundf(11.0f + t * 3.0f));
        for (int y = top; y <= bot; ++y)
            dot(x, y);
    }

    // 声波弧线（近似 C# DrawArc(11,4,3,8,-50,100) 的几个像素）
    static const int wX[] = {12, 13, 14, 14, 14, 13, 12};
    static const int wY[] = { 4,  5,  6,  7,  8,  9, 10};
    for (int i = 0; i < 7; ++i) dot(wX[i], wY[i]);

    // 掩码全零 → 使用 32bpp alpha 通道控制透明度
    const int maskRowBytes  = ((sz + 15) / 16) * 2;  // WORD 对齐
    std::vector<BYTE> maskData(static_cast<size_t>(maskRowBytes * sz), 0);
    HBITMAP  hMask = CreateBitmap(sz, sz, 1, 1, maskData.data());

    ICONINFO ii = {TRUE, 0, 0, hMask, hBmp};
    HICON icon  = CreateIconIndirect(&ii);
    DeleteObject(hMask);
    DeleteObject(hBmp);
    return icon;
}

// ============================================================
//  托盘图标管理
// ============================================================

static void TrayAdd() {
    NOTIFYICONDATA nid       = {};
    nid.cbSize               = sizeof(nid);
    nid.hWnd                 = g_hTray;
    nid.uID                  = TRAY_UID;
    nid.uFlags               = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage     = WM_TRAYNOTIFY;
    nid.hIcon                = g_hIcon;
    wcsncpy_s(nid.szTip, 128, L"音量平衡控制器", _TRUNCATE);
    Shell_NotifyIcon(NIM_ADD, &nid);
}

static void TrayRemove() {
    NOTIFYICONDATA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_hTray;
    nid.uID    = TRAY_UID;
    Shell_NotifyIcon(NIM_DELETE, &nid);
}

// ============================================================
//  弹窗：数据同步与标签刷新
// ============================================================

static void RefreshLabels() {
    wchar_t buf[32];

    int mv = static_cast<int>(SendMessage(g_sliders[SLD_MASTER], TBM_GETPOS, 0, 0));
    int lv = static_cast<int>(SendMessage(g_sliders[SLD_LEFT],   TBM_GETPOS, 0, 0));
    int rv = static_cast<int>(SendMessage(g_sliders[SLD_RIGHT],  TBM_GETPOS, 0, 0));

    swprintf_s(buf, L"%d%%", mv); SetWindowText(g_valLabels[SLD_MASTER], buf);
    swprintf_s(buf, L"%d",   lv); SetWindowText(g_valLabels[SLD_LEFT],   buf);
    swprintf_s(buf, L"%d",   rv); SetWindowText(g_valLabels[SLD_RIGHT],  buf);

    SetWindowText(g_hRatioLbl,
        (std::wstring(L"固化比例: ") + g_audio->GetRatioText()).c_str());
}

// 将音频控制器的比例因子同步到左右滑块
static void SyncSlidersToFactors() {
    g_syncing = true;
    auto setPos = [](HWND hw, float factor) {
        int v = std::clamp(static_cast<int>(std::roundf(factor * 100.0f)), 0, 100);
        SendMessage(hw, TBM_SETPOS, TRUE, static_cast<LPARAM>(v));
    };
    setPos(g_sliders[SLD_LEFT],  g_audio->GetLeftFactor());
    setPos(g_sliders[SLD_RIGHT], g_audio->GetRightFactor());
    g_syncing = false;
}

// 全量同步（弹窗首次显示 / 硬件音量变化时）
static void SyncAll() {
    SyncSlidersToFactors();
    g_syncing = true;
    int m = std::clamp(
        static_cast<int>(std::roundf(g_audio->GetMaster() * 100.0f)), 0, 100);
    SendMessage(g_sliders[SLD_MASTER], TBM_SETPOS, TRUE, static_cast<LPARAM>(m));
    g_syncing = false;
    RefreshLabels();
}

// ============================================================
//  弹窗：位置与可见性
// ============================================================

static void PositionPopup() {
    RECT work{};
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    RECT win{};
    GetWindowRect(g_hPopup, &win);
    int w = win.right - win.left;
    int h = win.bottom - win.top;
    SetWindowPos(g_hPopup, HWND_TOPMOST,
                 work.right - w - 12,
                 work.bottom - h - 12,
                 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static void ShowPopup() {
    SyncAll();
    PositionPopup();
    ShowWindow(g_hPopup, SW_SHOW);
    SetForegroundWindow(g_hPopup);
}

static void HidePopup() {
    ShowWindow(g_hPopup, SW_HIDE);
}

static void TogglePopup() {
    if (IsWindowVisible(g_hPopup)) HidePopup(); else ShowPopup();
}

// ============================================================
//  INI 持久化
//  文件位置：exe 同目录 BalancedVolume.ini
//  格式：
//    [Balance]
//    Left=67
//    Right=100
// ============================================================

static std::wstring GetIniPath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    wchar_t* sep = wcsrchr(buf, L'\\');
    if (sep) sep[1] = L'\0';   // 保留末尾反斜杠，截掉文件名
    return std::wstring(buf) + L"BalancedVolume.ini";
}

// 将当前比例因子以 0–100 整数写入 ini（方便手动编辑）
static void SaveIni() {
    const std::wstring path = GetIniPath();
    wchar_t val[16];
    int l = std::clamp(
        static_cast<int>(std::roundf(g_audio->GetLeftFactor()  * 100.0f)), 0, 100);
    int r = std::clamp(
        static_cast<int>(std::roundf(g_audio->GetRightFactor() * 100.0f)), 0, 100);
    swprintf_s(val, L"%d", l);
    WritePrivateProfileStringW(L"Balance", L"Left",  val, path.c_str());
    swprintf_s(val, L"%d", r);
    WritePrivateProfileStringW(L"Balance", L"Right", val, path.c_str());
}

// 读取并应用；ini 不存在时静默跳过，保持硬件当前状态
static void LoadIni() {
    const std::wstring path = GetIniPath();
    // 用 -1 作哨兵：若键不存在 GetPrivateProfileInt 返回默认值 -1
    int l = static_cast<int>(
        GetPrivateProfileIntW(L"Balance", L"Left",  -1, path.c_str()));
    int r = static_cast<int>(
        GetPrivateProfileIntW(L"Balance", L"Right", -1, path.c_str()));
    if (l < 0 || r < 0)   return;   // 文件或键不存在
    if (l == 0 && r == 0) return;   // 无效值
    g_audio->SetBalance(static_cast<float>(l), static_cast<float>(r));
}

// ============================================================
//  弹窗控件创建（对应 C# ControlForm 布局）
//
//  布局（客户区 320×200）:
//    y=12  [ 主音量 ][===slider===] 75%
//    y=58  [ 左声道 ][===slider===] 67
//    y=104 [ 右声道 ][===slider===] 100
//    y=152          固化比例: 2 : 3
// ============================================================

static void BuildPopupControls(HWND hWnd) {
    constexpr int col0 = 10, col1 = 68, col2 = 260;
    constexpr int rowH = 46, startY = 12;

    static const wchar_t* kRowLabels[] = {L"主音量", L"左声道", L"右声道"};

    for (int i = 0; i < SLD_COUNT; ++i) {
        int y = startY + i * rowH;

        // 行标题（右对齐）
        HWND hTitle = CreateWindowW(L"STATIC", kRowLabels[i],
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            col0, y + 8, 55, 30,
            hWnd, nullptr, g_hInst, nullptr);
        SendMessage(hTitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);

        // 滑块
        HWND hSld = CreateWindowW(TRACKBAR_CLASS, nullptr,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            col1, y + 2, 185, 30,
            hWnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SLD_MASTER + i)),
            g_hInst, nullptr);
        SendMessage(hSld, TBM_SETRANGE,    FALSE, MAKELONG(0, 100));
        SendMessage(hSld, TBM_SETPAGESIZE, 0,     10);
        g_sliders[i] = hSld;

        // 数值标签
        HWND hVal = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            col2, y + 8, 48, 30,
            hWnd, nullptr, g_hInst, nullptr);
        SendMessage(hVal, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        g_valLabels[i] = hVal;
    }

    // 比例说明标签
    int ry = startY + SLD_COUNT * rowH + 2;
    g_hRatioLbl = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, ry, 300, 22,
        hWnd, nullptr, g_hInst, nullptr);
    SendMessage(g_hRatioLbl, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
}

// ============================================================
//  弹窗窗口过程
// ============================================================

static LRESULT CALLBACK PopupProc(HWND hWnd, UINT msg,
                                   WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    // ── 创建 ────────────────────────────────────────────────────────────
    case WM_CREATE:
        BuildPopupControls(hWnd);
        return 0;

    // ── 背景（深色）────────────────────────────────────────────────────
    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hWnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, g_bgBrush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        FillRect(hdc, &ps.rcPaint, g_bgBrush);
        EndPaint(hWnd, &ps);
        return 0;
    }

    // ── STATIC 标签颜色（深底浅字）─────────────────────────────────────
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(220, 220, 220));
        SetBkColor(hdc, RGB(30, 30, 30));
        return reinterpret_cast<LRESULT>(g_bgBrush);
    }

    // ── 滑块变化（WM_HSCROLL 由子 TRACKBAR_CLASS 控件发给父窗口）───────
    //
    //  LOWORD(wParam) 通知码说明：
    //    SB_THUMBTRACK    — 鼠标按住拖动中（实时）
    //    SB_ENDSCROLL     — 鼠标松开 / 键盘键释放（操作结束）
    //    SB_LINE*/SB_PAGE* — 键盘箭头/翻页（每次按下触发一次）
    //
    //  策略：所有通知码都实时更新音频；仅在 SB_ENDSCROLL 时保存 ini，
    //        避免拖动过程中频繁写文件。
    case WM_HSCROLL: {
        if (g_syncing) return 0;

        HWND hSld = reinterpret_cast<HWND>(lParam);
        int  id   = GetDlgCtrlID(hSld);
        int  code = LOWORD(wParam);

        if (id == ID_SLD_MASTER) {
            // 主音量不写 ini（主音量由系统自己持久化）
            if (code != SB_ENDSCROLL) {
                int v = static_cast<int>(SendMessage(hSld, TBM_GETPOS, 0, 0));
                g_audio->SetMasterVolume(v / 100.0f);
                RefreshLabels();
            }
        } else if (id == ID_SLD_LEFT || id == ID_SLD_RIGHT) {
            if (code != SB_ENDSCROLL) {
                // 拖动 / 按键期间：实时更新音频
                int l = static_cast<int>(
                    SendMessage(g_sliders[SLD_LEFT],  TBM_GETPOS, 0, 0));
                int r = static_cast<int>(
                    SendMessage(g_sliders[SLD_RIGHT], TBM_GETPOS, 0, 0));
                g_audio->SetBalance(static_cast<float>(l),
                                     static_cast<float>(r));
                SyncSlidersToFactors();   // 归一化后同步回滑块
                RefreshLabels();
            } else {
                // 松开时：保存比例到 ini
                SaveIni();
            }
        }
        return 0;
    }

    // ── 失去焦点自动隐藏（对应 C# Deactivate 事件）─────────────────────
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
            HidePopup();
        return 0;

    // ── 点关闭只隐藏，不销毁（托盘应用生命周期）────────────────────────
    case WM_CLOSE:
        HidePopup();
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ============================================================
//  托盘宿主窗口过程（隐藏，仅接收消息）
// ============================================================

static LRESULT CALLBACK TrayProc(HWND hWnd, UINT msg,
                                  WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    // ── 托盘图标事件 ────────────────────────────────────────────────────
    case WM_TRAYNOTIFY:
        switch (lParam) {
        case WM_LBUTTONUP:
            TogglePopup();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            // SetForegroundWindow 必须在 TrackPopupMenu 之前调用，
            // 否则菜单在点击外部区域后不能正常消失
            POINT pt{};
            GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING,    IDM_OPEN, L"\u2699 \u6253\u5f00\u63a7\u5236\u9762\u677f");  // ⚙ 打开控制面板
            AppendMenuW(hMenu, MF_SEPARATOR, 0,        nullptr);
            AppendMenuW(hMenu, MF_STRING,    IDM_EXIT, L"\u2715 \u9000\u51fa");  // ✕ 退出
            TrackPopupMenu(hMenu,
                           TPM_BOTTOMALIGN | TPM_RIGHTALIGN | TPM_RIGHTBUTTON,
                           pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(hMenu);
            // PostMessage(WM_NULL) 修正：确保宿主窗口能正确恢复前景状态
            PostMessage(hWnd, WM_NULL, 0, 0);
            break;
        }
        }
        return 0;

    // ── 菜单命令 ────────────────────────────────────────────────────────
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_OPEN: TogglePopup(); break;
        case IDM_EXIT:
            TrayRemove();
            PostQuitMessage(0);
            break;
        }
        return 0;

    // ── 音频线程通知：硬件音量被外部改变，刷新弹窗 ─────────────────────
    case WM_SYNC_UI:
        if (IsWindowVisible(g_hPopup))
            SyncAll();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ============================================================
//  WinMain
// ============================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    g_hInst = hInstance;

    // COM 初始化（STA，与 C# Application.Run 一致）
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // 初始化公共控件（需要 TRACKBAR_CLASS）
    INITCOMMONCONTROLSEX icex{sizeof(icex), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icex);

    // ── 共享 GDI 资源 ────────────────────────────────────────────────────
    g_bgBrush = CreateSolidBrush(RGB(30, 30, 30));
    {
        HDC dc = GetDC(nullptr);
        // 9pt Microsoft YaHei UI，与 C# new Font("Microsoft YaHei UI", 9f) 对应
        g_uiFont = CreateFontW(
            -MulDiv(9, GetDeviceCaps(dc, LOGPIXELSY), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
        ReleaseDC(nullptr, dc);
    }
    g_hIcon = CreateSpeakerIcon();

    // ── 注册窗口类 ───────────────────────────────────────────────────────
    WNDCLASSEXW wc{};
    wc.cbSize    = sizeof(wc);
    wc.hInstance = hInstance;
    wc.hCursor   = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon     = g_hIcon;

    // 托盘宿主（隐藏）
    wc.lpfnWndProc   = TrayProc;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kTrayClass;
    RegisterClassExW(&wc);

    // 弹窗
    wc.lpfnWndProc   = PopupProc;
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = kPopupClass;
    RegisterClassExW(&wc);

    // ── 创建托盘宿主（WS_POPUP + 零尺寸 = 不可见，但可接收消息）────────
    g_hTray = CreateWindowExW(
        WS_EX_TOOLWINDOW,        // 不出现在任务栏
        kTrayClass, L"BalancedVolume",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hTray) { CoUninitialize(); return 1; }

    // ── 创建弹窗（初始隐藏）─────────────────────────────────────────────
    {
        constexpr DWORD style   = WS_POPUP | WS_BORDER;
        constexpr DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
        RECT rc = {0, 0, kPW, kPH};
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);  // 将客户区尺寸转为窗口尺寸

        g_hPopup = CreateWindowExW(
            exStyle, kPopupClass, L"音量平衡控制器",
            style,
            0, 0,
            rc.right - rc.left,   // 窗口宽（含边框）
            rc.bottom - rc.top,   // 窗口高（含边框）
            nullptr, nullptr, hInstance, nullptr);
    }

    if (!g_hPopup) { CoUninitialize(); return 1; }

    // ── 初始化音频控制器 ─────────────────────────────────────────────────
    g_audio = new AudioController();
    if (!g_audio->Initialize()) {
        MessageBoxW(nullptr,
            L"无法访问默认音频输出端点。\n请检查系统音频设置后重试。",
            L"BalancedVolume 错误",
            MB_ICONERROR | MB_OK);
        delete g_audio;
        CoUninitialize();
        return 1;
    }

    // ini 存在则用保存的比例覆盖硬件当前状态（在注册回调前调用，
    // 避免 SetBalance 触发 OnStateChanged 时 g_hTray 尚未准备好）
    LoadIni();

    // 音频线程回调 → PostMessage → UI 线程处理（不直接操作 HWND）
    g_audio->OnStateChanged = [] {
        PostMessage(g_hTray, WM_SYNC_UI, 0, 0);
    };

    // ── 添加托盘图标 ─────────────────────────────────────────────────────
    TrayAdd();

    // ── 消息循环 ─────────────────────────────────────────────────────────
    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // ── 清理（COM 注销必须在 CoUninitialize 之前）────────────────────────
    TrayRemove();
    delete g_audio;      // 析构中 UnregisterControlChangeNotify 并释放 COM 接口
    g_audio = nullptr;

    if (g_uiFont)  DeleteObject(g_uiFont);
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_hIcon)   DestroyIcon(g_hIcon);

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
