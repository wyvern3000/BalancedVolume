/*
 * BalancedVolume — 固化左右声道比例的托盘工具（多设备版）
 *
 * 弹窗布局（320×220 客户区）：
 *   y=10  [☑ 跟随默认设备]  [设备名称下拉框          ▾]
 *   y=50  主音量  [slider]  75%
 *   y=96  左声道  [slider]  67
 *   y=142 右声道  [slider]  100
 *   y=192            固化比例: 2 : 3
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
//  常量
// ============================================================

static const wchar_t kTrayClass[]  = L"BV_TrayHost";
static const wchar_t kPopupClass[] = L"BV_Popup";

static constexpr UINT WM_TRAYNOTIFY    = WM_APP + 1;
static constexpr UINT WM_SYNC_UI       = WM_APP + 2;
static constexpr UINT WM_DEVICE_CHANGE = WM_APP + 3;
static constexpr UINT TRAY_UID         = 1;

static constexpr int kPW = 320;   // 弹窗客户区宽
static constexpr int kPH = 220;   // 弹窗客户区高（比旧版多 20px）

// 布局 Y 坐标
static constexpr int kDeviceRowY  = 10;  // 设备选择行
static constexpr int kSliderStartY = 50; // 第一个滑块行

static constexpr int kRowH = 46;         // 滑块行高（不变）

// 菜单 ID
enum : UINT { IDM_OPEN = 100, IDM_EXIT };

// 滑块索引
enum : int { SLD_MASTER = 0, SLD_LEFT, SLD_RIGHT, SLD_COUNT };

// 控件 ID
enum : int {
    ID_SLD_MASTER = 200, ID_SLD_LEFT, ID_SLD_RIGHT,
    ID_CHK_FOLLOW = 300,
    ID_CMB_DEVICE = 301,
};

// ============================================================
//  全局状态
// ============================================================

static HINSTANCE        g_hInst        = nullptr;
static HWND             g_hTray        = nullptr;
static HWND             g_hPopup       = nullptr;
static HICON            g_hIcon        = nullptr;
static AudioController* g_audio        = nullptr;

static bool g_syncing      = false;
static bool g_followDefault = true;

// 弹窗控件
static HWND g_sliders[SLD_COUNT];
static HWND g_valLabels[SLD_COUNT];
static HWND g_hRatioLbl   = nullptr;
static HWND g_hFollowChk  = nullptr;
static HWND g_hDeviceCombo = nullptr;

// 共享 GDI
static HBRUSH g_bgBrush = nullptr;
static HFONT  g_uiFont  = nullptr;

// ============================================================
//  图标（与之前版本完全相同）
// ============================================================

static HICON CreateSpeakerIcon() {
    constexpr int sz = 16;
    BITMAPV5HEADER bi  = {};
    bi.bV5Size         = sizeof(bi);
    bi.bV5Width        = sz;
    bi.bV5Height       = -sz;
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

    auto* p = static_cast<UINT32*>(pBits);
    std::fill(p, p + sz * sz, 0u);

    auto dot = [&](int x, int y) noexcept {
        if (static_cast<unsigned>(x) < static_cast<unsigned>(sz) &&
            static_cast<unsigned>(y) < static_cast<unsigned>(sz))
            p[y * sz + x] = 0xFFFFFFFFu;
    };

    for (int y = 5; y <= 10; ++y)
        for (int x = 2; x <= 5; ++x)
            dot(x, y);

    for (int x = 5; x <= 10; ++x) {
        float t   = static_cast<float>(x - 5) / 5.0f;
        int   top = static_cast<int>(std::roundf(4.0f - t * 3.0f));
        int   bot = static_cast<int>(std::roundf(11.0f + t * 3.0f));
        for (int y = top; y <= bot; ++y) dot(x, y);
    }

    static const int wX[] = {12, 13, 14, 14, 14, 13, 12};
    static const int wY[] = { 4,  5,  6,  7,  8,  9, 10};
    for (int i = 0; i < 7; ++i) dot(wX[i], wY[i]);

    const int maskRowBytes = ((sz + 15) / 16) * 2;
    std::vector<BYTE> maskData(static_cast<size_t>(maskRowBytes * sz), 0);
    HBITMAP  hMask = CreateBitmap(sz, sz, 1, 1, maskData.data());
    ICONINFO ii    = {TRUE, 0, 0, hMask, hBmp};
    HICON    icon  = CreateIconIndirect(&ii);
    DeleteObject(hMask);
    DeleteObject(hBmp);
    return icon;
}

// ============================================================
//  托盘
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
//  INI 持久化
//
//  [General]
//  FollowDefault=1
//  ActiveDevice={0.0.0.00000000}.{...}
//
//  [Device.{0.0.0.00000000}.{...}]
//  Name=USB Audio Device
//  Left=67
//  Right=100
// ============================================================

static std::wstring GetIniPath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    wchar_t* sep = wcsrchr(buf, L'\\');
    if (sep) sep[1] = L'\0';
    return std::wstring(buf) + L"BalancedVolume.ini";
}

// 保存 [General] 节
static void SaveGeneralSettings() {
    const std::wstring path = GetIniPath();
    WritePrivateProfileStringW(L"General", L"FollowDefault",
        g_followDefault ? L"1" : L"0", path.c_str());
    if (!g_followDefault) {
        const std::wstring id = g_audio->GetCurrentDeviceId();
        if (!id.empty())
            WritePrivateProfileStringW(L"General", L"ActiveDevice",
                id.c_str(), path.c_str());
    }
}

// 保存当前设备的平衡到对应 [Device.{id}] 节
static void SaveDeviceBalance() {
    const std::wstring path    = GetIniPath();
    const std::wstring id      = g_audio->GetCurrentDeviceId();
    const std::wstring name    = g_audio->GetCurrentDeviceName();
    const std::wstring section = L"Device." + id;
    wchar_t val[16];

    WritePrivateProfileStringW(section.c_str(), L"Name",
        name.c_str(), path.c_str());

    int l = std::clamp(
        static_cast<int>(std::roundf(g_audio->GetLeftFactor()  * 100.0f)), 0, 100);
    int r = std::clamp(
        static_cast<int>(std::roundf(g_audio->GetRightFactor() * 100.0f)), 0, 100);
    swprintf_s(val, L"%d", l);
    WritePrivateProfileStringW(section.c_str(), L"Left",  val, path.c_str());
    swprintf_s(val, L"%d", r);
    WritePrivateProfileStringW(section.c_str(), L"Right", val, path.c_str());
}

// 从 [Device.{id}] 节加载并应用平衡；找不到则默认 1:1
static void LoadDeviceBalance(const std::wstring& deviceId) {
    const std::wstring path    = GetIniPath();
    const std::wstring section = L"Device." + deviceId;
    int l = static_cast<int>(
        GetPrivateProfileIntW(section.c_str(), L"Left",  -1, path.c_str()));
    int r = static_cast<int>(
        GetPrivateProfileIntW(section.c_str(), L"Right", -1, path.c_str()));
    if (l >= 0 && r >= 0 && (l > 0 || r > 0))
        g_audio->SetBalance(static_cast<float>(l), static_cast<float>(r));
    else
        g_audio->SetBalance(100.0f, 100.0f);  // 该设备无记录，默认 1:1
}

// 启动时读取 [General] 并完成初始设备绑定
static void LoadGeneralSettings() {
    const std::wstring path = GetIniPath();

    // FollowDefault 默认为 true（首次启动无 ini）
    int follow = static_cast<int>(
        GetPrivateProfileIntW(L"General", L"FollowDefault", 1, path.c_str()));
    g_followDefault = (follow != 0);

    if (g_followDefault) {
        // 绑定到默认设备（Initialize() 已完成，这里只加载平衡）
        LoadDeviceBalance(g_audio->GetCurrentDeviceId());
    } else {
        // 尝试绑定到保存的设备
        wchar_t idBuf[512] = {};
        GetPrivateProfileStringW(L"General", L"ActiveDevice", L"",
            idBuf, 512, path.c_str());

        if (idBuf[0] != L'\0') {
            bool found = g_audio->BindToDevice(idBuf);
            if (!found) {
                // 设备未连接，已自动回退到默认设备
                // 保持 g_followDefault = false（下次接入时再生效）
            }
        }
        LoadDeviceBalance(g_audio->GetCurrentDeviceId());
    }
}

// ============================================================
//  弹窗：设备下拉框同步
// ============================================================

// 填充设备列表并选中当前设备；每次 ShowPopup 调用一次
static void SyncDeviceCombo() {
    SendMessage(g_hDeviceCombo, CB_RESETCONTENT, 0, 0);

    const std::vector<DeviceInfo> devices = g_audio->EnumerateDevices();
    const std::wstring curId = g_audio->GetCurrentDeviceId();
    int selectIdx = 0;

    for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
        const auto& d = devices[i];
        // 默认设备在名称后加星号标注
        std::wstring label = d.isDefault ? (d.name + L"  ★") : d.name;
        int idx = static_cast<int>(
            SendMessage(g_hDeviceCombo, CB_ADDSTRING, 0,
                        reinterpret_cast<LPARAM>(label.c_str())));
        // 用 CB_SETITEMDATA 存设备索引，以便通过 combo index 反查 devices[]
        SendMessage(g_hDeviceCombo, CB_SETITEMDATA, static_cast<WPARAM>(idx),
                    static_cast<LPARAM>(i));
        if (d.id == curId) selectIdx = idx;
    }

    SendMessage(g_hDeviceCombo, CB_SETCURSEL,
                static_cast<WPARAM>(selectIdx), 0);

    // 跟随默认时禁用下拉（只能看到当前默认，不能手动切换）
    EnableWindow(g_hDeviceCombo, g_followDefault ? FALSE : TRUE);

    // 同步复选框状态
    SendMessage(g_hFollowChk, BM_SETCHECK,
                g_followDefault ? BST_CHECKED : BST_UNCHECKED, 0);
}

// ============================================================
//  弹窗：平衡滑块同步
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

static void SyncSlidersToFactors() {
    g_syncing = true;
    auto setPos = [](HWND hw, float factor) {
        int v = std::clamp(
            static_cast<int>(std::roundf(factor * 100.0f)), 0, 100);
        SendMessage(hw, TBM_SETPOS, TRUE, static_cast<LPARAM>(v));
    };
    setPos(g_sliders[SLD_LEFT],  g_audio->GetLeftFactor());
    setPos(g_sliders[SLD_RIGHT], g_audio->GetRightFactor());
    g_syncing = false;
}

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
    SetWindowPos(g_hPopup, HWND_TOPMOST,
                 work.right  - (win.right  - win.left) - 12,
                 work.bottom - (win.bottom - win.top)  - 12,
                 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static void ShowPopup() {
    SyncDeviceCombo();   // 每次弹出时刷新设备列表
    SyncAll();
    PositionPopup();
    ShowWindow(g_hPopup, SW_SHOW);
    SetForegroundWindow(g_hPopup);
}

static void HidePopup() { ShowWindow(g_hPopup, SW_HIDE); }

static void TogglePopup() {
    if (IsWindowVisible(g_hPopup)) HidePopup(); else ShowPopup();
}

// ============================================================
//  弹窗控件创建
// ============================================================

static void BuildPopupControls(HWND hWnd) {
    // ── 设备选择行 ─────────────────────────────────────────────────────────
    g_hFollowChk = CreateWindowW(L"BUTTON", L"跟随默认设备",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        10, kDeviceRowY + 4, 110, 20,
        hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHK_FOLLOW)),
        g_hInst, nullptr);
    SendMessage(g_hFollowChk, WM_SETFONT,
                reinterpret_cast<WPARAM>(g_uiFont), TRUE);

    g_hDeviceCombo = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_TABSTOP,
        124, kDeviceRowY, 188, 200,   // 高度 200 = 下拉列表最大高度
        hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CMB_DEVICE)),
        g_hInst, nullptr);
    SendMessage(g_hDeviceCombo, WM_SETFONT,
                reinterpret_cast<WPARAM>(g_uiFont), TRUE);

    // ── 三行滑块 ────────────────────────────────────────────────────────────
    static const wchar_t* kLabels[] = {L"主音量", L"左声道", L"右声道"};

    for (int i = 0; i < SLD_COUNT; ++i) {
        int y = kSliderStartY + i * kRowH;

        HWND hTitle = CreateWindowW(L"STATIC", kLabels[i],
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            10, y + 8, 55, 30,
            hWnd, nullptr, g_hInst, nullptr);
        SendMessage(hTitle, WM_SETFONT,
                    reinterpret_cast<WPARAM>(g_uiFont), TRUE);

        HWND hSld = CreateWindowW(TRACKBAR_CLASS, nullptr,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            68, y + 2, 185, 30,
            hWnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SLD_MASTER + i)),
            g_hInst, nullptr);
        SendMessage(hSld, TBM_SETRANGE,    FALSE, MAKELONG(0, 100));
        SendMessage(hSld, TBM_SETPAGESIZE, 0,     10);
        g_sliders[i] = hSld;

        HWND hVal = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            260, y + 8, 48, 30,
            hWnd, nullptr, g_hInst, nullptr);
        SendMessage(hVal, WM_SETFONT,
                    reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        g_valLabels[i] = hVal;
    }

    // ── 比例说明标签 ────────────────────────────────────────────────────────
    int ry = kSliderStartY + SLD_COUNT * kRowH + 2;
    g_hRatioLbl = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, ry, 300, 22,
        hWnd, nullptr, g_hInst, nullptr);
    SendMessage(g_hRatioLbl, WM_SETFONT,
                reinterpret_cast<WPARAM>(g_uiFont), TRUE);
}

// ============================================================
//  弹窗窗口过程
// ============================================================

static LRESULT CALLBACK PopupProc(HWND hWnd, UINT msg,
                                   WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_CREATE:
        BuildPopupControls(hWnd);
        return 0;

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

    // Static 标签：深底浅字
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(220, 220, 220));
        SetBkColor(hdc, RGB(30, 30, 30));
        return reinterpret_cast<LRESULT>(g_bgBrush);
    }
    // 复选框同上
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(220, 220, 220));
        SetBkColor(hdc, RGB(30, 30, 30));
        return reinterpret_cast<LRESULT>(g_bgBrush);
    }

    // ── 滑块 ──────────────────────────────────────────────────────────────
    case WM_HSCROLL: {
        if (g_syncing) return 0;
        HWND hSld = reinterpret_cast<HWND>(lParam);
        int  id   = GetDlgCtrlID(hSld);
        int  code = LOWORD(wParam);

        if (id == ID_SLD_MASTER) {
            if (code != SB_ENDSCROLL) {
                int v = static_cast<int>(SendMessage(hSld, TBM_GETPOS, 0, 0));
                g_audio->SetMasterVolume(v / 100.0f);
                RefreshLabels();
            }
        } else if (id == ID_SLD_LEFT || id == ID_SLD_RIGHT) {
            if (code != SB_ENDSCROLL) {
                int l = static_cast<int>(
                    SendMessage(g_sliders[SLD_LEFT],  TBM_GETPOS, 0, 0));
                int r = static_cast<int>(
                    SendMessage(g_sliders[SLD_RIGHT], TBM_GETPOS, 0, 0));
                g_audio->SetBalance(static_cast<float>(l),
                                     static_cast<float>(r));
                SyncSlidersToFactors();
                RefreshLabels();
            } else {
                SaveDeviceBalance();   // 松开时保存到当前设备的 ini 节
            }
        }
        return 0;
    }

    // ── 复选框 / 下拉框 ────────────────────────────────────────────────────
    case WM_COMMAND: {
        int id   = LOWORD(wParam);
        int code = HIWORD(wParam);

        // "跟随默认设备" 复选框
        if (id == ID_CHK_FOLLOW && code == BN_CLICKED) {
            g_followDefault =
                (SendMessage(g_hFollowChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(g_hDeviceCombo, g_followDefault ? FALSE : TRUE);

            if (g_followDefault) {
                // 立即切到默认设备
                g_audio->BindToDevice(L"");
                LoadDeviceBalance(g_audio->GetCurrentDeviceId());
                SyncDeviceCombo();
                SyncAll();
            }
            SaveGeneralSettings();
        }

        // 设备下拉框选择变化
        else if (id == ID_CMB_DEVICE && code == CBN_SELCHANGE) {
            int idx = static_cast<int>(
                SendMessage(g_hDeviceCombo, CB_GETCURSEL, 0, 0));
            if (idx == CB_ERR) break;

            // 取出该条目存的 devices[] 下标，再从 EnumerateDevices 重查 ID
            // （简化：直接重新枚举，因为 EnumerateDevices 很快）
            auto devices = g_audio->EnumerateDevices();
            int  dataIdx = static_cast<int>(
                SendMessage(g_hDeviceCombo, CB_GETITEMDATA,
                            static_cast<WPARAM>(idx), 0));

            if (dataIdx >= 0 && dataIdx < static_cast<int>(devices.size())) {
                const std::wstring& newId = devices[dataIdx].id;
                if (newId != g_audio->GetCurrentDeviceId()) {
                    g_audio->BindToDevice(newId);
                    LoadDeviceBalance(newId);
                    SyncAll();
                    SaveGeneralSettings();
                }
            }
        }
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) HidePopup();
        return 0;

    case WM_CLOSE:
        HidePopup();
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ============================================================
//  托盘宿主窗口过程
// ============================================================

static LRESULT CALLBACK TrayProc(HWND hWnd, UINT msg,
                                  WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_TRAYNOTIFY:
        switch (lParam) {
        case WM_LBUTTONUP:
            TogglePopup();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            POINT pt{};
            GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING,    IDM_OPEN,
                        L"\u2699 \u6253\u5f00\u63a7\u5236\u9762\u677f");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING,    IDM_EXIT,
                        L"\u2715 \u9000\u51fa");
            TrackPopupMenu(hMenu,
                TPM_BOTTOMALIGN | TPM_RIGHTALIGN | TPM_RIGHTBUTTON,
                pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(hMenu);
            PostMessage(hWnd, WM_NULL, 0, 0);
            break;
        }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_OPEN: TogglePopup(); break;
        case IDM_EXIT:
            TrayRemove();
            PostQuitMessage(0);
            break;
        }
        return 0;

    // 音频线程通知：当前设备音量被外部改变
    case WM_SYNC_UI:
        if (IsWindowVisible(g_hPopup)) SyncAll();
        return 0;

    // 系统默认播放设备已切换
    case WM_DEVICE_CHANGE:
        if (g_followDefault) {
            g_audio->BindToDevice(L"");                        // 绑定到新默认
            LoadDeviceBalance(g_audio->GetCurrentDeviceId());  // 加载该设备的平衡
            if (IsWindowVisible(g_hPopup)) {
                SyncDeviceCombo();
                SyncAll();
            }
        }
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

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icex{sizeof(icex), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icex);

    // 共享 GDI 资源
    g_bgBrush = CreateSolidBrush(RGB(30, 30, 30));
    {
        HDC dc = GetDC(nullptr);
        g_uiFont = CreateFontW(
            -MulDiv(9, GetDeviceCaps(dc, LOGPIXELSY), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
        ReleaseDC(nullptr, dc);
    }
    g_hIcon = CreateSpeakerIcon();

    // 注册窗口类
    WNDCLASSEXW wc{};
    wc.cbSize    = sizeof(wc);
    wc.hInstance = hInstance;
    wc.hCursor   = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon     = g_hIcon;

    wc.lpfnWndProc   = TrayProc;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kTrayClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = PopupProc;
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = kPopupClass;
    RegisterClassExW(&wc);

    // 隐藏托盘宿主
    g_hTray = CreateWindowExW(WS_EX_TOOLWINDOW,
        kTrayClass, L"BalancedVolume", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!g_hTray) { CoUninitialize(); return 1; }

    // 弹窗（客户区 320×220）
    {
        constexpr DWORD style   = WS_POPUP | WS_BORDER;
        constexpr DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
        RECT rc = {0, 0, kPW, kPH};
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
        g_hPopup = CreateWindowExW(exStyle, kPopupClass, L"音量平衡控制器",
            style, 0, 0,
            rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, hInstance, nullptr);
    }
    if (!g_hPopup) { CoUninitialize(); return 1; }

    // 音频初始化
    g_audio = new AudioController();
    if (!g_audio->Initialize()) {
        MessageBoxW(nullptr,
            L"无法访问默认音频输出端点。\n请检查系统音频设置后重试。",
            L"BalancedVolume 错误", MB_ICONERROR | MB_OK);
        delete g_audio;
        CoUninitialize();
        return 1;
    }

    // 读取 ini：确定跟随模式、绑定目标设备、加载平衡
    LoadGeneralSettings();

    // 线程安全回调：音频线程 → PostMessage → UI 线程
    g_audio->OnStateChanged = [] {
        PostMessage(g_hTray, WM_SYNC_UI, 0, 0);
    };
    g_audio->OnDeviceChanged = [] {
        PostMessage(g_hTray, WM_DEVICE_CHANGE, 0, 0);
    };

    TrayAdd();

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    TrayRemove();
    delete g_audio;
    g_audio = nullptr;

    if (g_uiFont)  DeleteObject(g_uiFont);
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_hIcon)   DestroyIcon(g_hIcon);

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
