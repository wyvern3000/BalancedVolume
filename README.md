# BalancedVolume

固化 Windows 左右声道音量比例的轻量托盘工具。

当你用键盘音量键或滚轮调节主音量时，左右声道比例自动保持不变，
适合用于矫正耳机/音箱左右声道不平衡的场景。

---

## 功能

- 系统托盘常驻，零界面打扰
- 左右声道比例锁定，主音量上下拉动不影响比例
- 比例设置持久化保存（`BalancedVolume.ini`），重启后自动恢复
- 纯 Win32 原生实现，无需安装运行库

---

## 系统要求

- Windows 10 / 11（Vista 及以上理论可用，需 WASAPI 支持）
- 默认音频输出设备为立体声（双声道）

---

## 使用方法

将 `BalancedVolume.exe` 放到任意目录，双击启动，托盘区会出现扬声器图标。

### 托盘图标操作

| 操作 | 效果 |
|------|------|
| 左键单击 | 打开 / 关闭控制面板 |
| 右键单击 | 菜单：打开控制面板 / 退出 |

### 控制面板

弹出于屏幕右下角，点击其他区域自动关闭。

```
┌──────────────────────────────────────┐
│  主音量  [══════════════════]  75%   │
│  左声道  [═══════════       ]  67    │
│  右声道  [════════════════  ]  100   │
│                                      │
│           固化比例: 2 : 3            │
└──────────────────────────────────────┘
```

**主音量**：等同于系统音量，拉动时左右比例保持不变。

**左声道 / 右声道**：设定比例的两侧权重，程序内部取较大值为基准进行归一化。
例如，将左声道拉到 `67`、右声道拉到 `100`，固化比例显示为 `2 : 3`，
此后无论主音量如何变化，左声道始终为右声道的 ²⁄₃。

**松开**左右声道滑块后，比例自动写入 `BalancedVolume.ini`。

---

## 配置文件

程序在 **exe 同目录**自动生成 `BalancedVolume.ini`，可手动编辑：

```ini
[Balance]
Left=67
Right=100
```

- 两个值均为 0–100 的整数，只有比值有意义（`Left=34, Right=50` 与 `Left=67, Right=100` 等价）
- 文件不存在时，程序读取系统当前声道状态作为初始比例
- 修改后下次启动生效；运行中不会自动重载

---

## 从源码编译

### 工具链

推荐 [winlibs](https://winlibs.com/) 提供的 MinGW-w64 UCRT 版本（gcc 16+），
或任意支持 C++17 的 MinGW-w64 x86-64 工具链。

### 文件清单

```
audio.h        AudioController 类声明
audio.cpp      WASAPI COM 实现
main.cpp       Win32 托盘窗口 + 控制面板
resource.rc    图标资源绑定
icon.ico       程序图标（16×16 + 32×32）
Makefile       构建脚本
```

### 构建

```bash
make
```

或手动执行：

```bash
# 编译图标资源
windres resource.rc -o resource.o

# 编译并链接
g++ -std=c++17 -O2 -Wall -Wextra \
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
    -mwindows -static-libgcc -static-libstdc++ \
    -o BalancedVolume.exe main.cpp audio.cpp resource.o \
    -lole32 -luuid -lshell32 -lcomctl32 -lgdi32 -luser32
```

链接的系统库说明：

| 库 | 用途 |
|----|------|
| `ole32` | COM 初始化、`CoCreateInstance` |
| `uuid` | 基础 COM 接口 GUID（`IID_IUnknown` 等） |
| `shell32` | 托盘图标 `Shell_NotifyIcon` |
| `comctl32` | 滑块控件 `TRACKBAR_CLASS` |
| `gdi32` | 字体、画刷、图标位图 |
| `user32` | 窗口消息、菜单 |

---

## 技术说明

### 声道控制

通过 Windows Core Audio API（WASAPI）的 `IAudioEndpointVolume` 接口控制声道音量。
注册 `IAudioEndpointVolumeCallback` 监听硬件音量变化（键盘音量键、系统设置等），
在回调中重新应用比例，保证比例不被外部操作打破。

### 线程安全

WASAPI 回调在音频线程触发，通过 `PostMessage` 切回 UI 线程再刷新界面，
避免跨线程直接操作 HWND。音频操作期间用原子标志 `_isBusy` 防止回调重入。

### 持久化机制

Windows 会将每个音频端点的声道音量保存在注册表中，因此在没有 ini 文件的情况下，
重启程序也能从系统状态推算出上次的比例——但当主音量为 0（静音）时此推算会失效，
ini 文件解决了这一边界情况。
