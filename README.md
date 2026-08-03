# BalancedVolume

固化 Windows 左右声道音量比例的轻量托盘工具。

当你用键盘音量键或滚轮调节主音量时，左右声道比例自动保持不变，
适合用于矫正耳机/音箱左右声道不平衡的场景。

---

## 功能

- 系统托盘常驻，零界面打扰
- 左右声道比例锁定，主音量上下拉动不影响比例
- 支持多音频设备：可固定控制某台设备，或跟随系统默认设备自动切换
- 每台设备独立保存比例，切换设备后自动恢复各自的设置
- 纯 Win32 原生实现，无需安装运行库

---

## 系统要求

- Windows 10 / 11（Vista 及以上理论可用，需 WASAPI 支持）
- 目标音频输出设备为立体声（双声道）

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
│  ☑ 跟随默认设备  [USB Audio    ▾]   │
├──────────────────────────────────────┤
│  主音量  [══════════════════]  75%   │
│  左声道  [═══════════       ]  67    │
│  右声道  [════════════════  ]  100   │
│           固化比例: 2 : 3            │
└──────────────────────────────────────┘
```

**跟随默认设备**：勾选后程序自动绑定系统当前默认播放设备，当系统默认设备切换时（如拔插耳机、在系统设置里切换）程序随之跟过去。取消勾选后可在下拉框中手动指定要控制的设备，其他设备不受影响。

**主音量**：等同于系统音量，拉动时左右比例保持不变。

**左声道 / 右声道**：设定比例的两侧权重，程序内部取较大值为基准进行归一化。
例如，将左声道拉到 `67`、右声道拉到 `100`，固化比例显示为 `2 : 3`，
此后无论主音量如何变化，左声道始终为右声道的 ²⁄₃。

**松开**左右声道滑块后，比例自动写入 `BalancedVolume.ini`，与当前设备绑定。

---

## 配置文件

程序在 **exe 同目录**自动生成 `BalancedVolume.ini`，可手动编辑：

```ini
[General]
FollowDefault=1
ActiveDevice={0.0.0.00000000}.{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}

[Device.{0.0.0.00000000}.{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}]
Name=USB Audio Device
Left=67
Right=100

[Device.{0.0.0.00000000}.{yyyyyyyy-yyyy-yyyy-yyyy-yyyyyyyyyyyy}]
Name=Realtek HD Audio
Left=88
Right=100
```

- `[General]` 保存「是否跟随默认」和上次使用的设备 ID
- `[Device.{id}]` 每台设备独立一节，`Name` 仅供人阅读，程序不使用
- `Left` / `Right` 为 0–100 整数，只有比值有意义
- 设备 ID 来自 Windows `IMMDevice::GetId()`，设备重插后不变
- 文件不存在时默认跟随默认设备，比例从系统当前声道状态读取
- 修改后下次启动生效；运行中不会自动重载

---

## 从源码编译

### 工具链

推荐 [winlibs](https://winlibs.com/) 提供的 MinGW-w64 UCRT 版本（gcc 16+），
或任意支持 C++17 的 MinGW-w64 x86-64 工具链。

### 文件清单

```
audio.h        AudioController 类声明
audio.cpp      WASAPI COM 实现（含多设备管理）
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

### 多设备支持

实现 `IMMNotificationClient` 的 `OnDefaultDeviceChanged` 回调监听系统默认设备变化。
「跟随默认设备」模式下，切换事件通过 `PostMessage` 发回 UI 线程，重新调用
`BindToDevice()` 绑定新设备并加载其对应的 INI 节。

### 线程安全

WASAPI 的音量回调和设备通知均在音频线程触发，统一通过 `PostMessage` 切回 UI
线程后再操作窗口控件和 INI 文件，避免跨线程竞争。音频写操作期间用原子标志
`_isBusy` 防止回调重入。

### 持久化机制

每台设备的比例以设备 ID 为 key 独立存储。Windows 自身会把声道音量持久化在注册表，
因此首次启动或 INI 不存在时，程序能从系统状态推算出当前比例作为初始值——但主音量
为 0（静音）时此推算会失效，INI 文件解决了这一边界情况。
