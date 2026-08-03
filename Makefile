# BalancedVolume — MinGW-w64 UCRT 构建配置
# 工具链: winlibs gcc-16.1.0-mingw-w64ucrt（或任意 MinGW-w64 UCRT 版本）
#
# 用法:
#   make          构建 BalancedVolume.exe
#   make clean    删除生成文件
#   make run      构建并运行（Wine 或 Windows 环境）

CXX     = g++
WINDRES = windres

# ── 编译标志 ──────────────────────────────────────────────────────────────────
CXXFLAGS = \
    -std=c++17 \
    -O2 \
    -Wall \
    -Wextra \
    -DUNICODE \
    -D_UNICODE \
    -DWIN32_LEAN_AND_MEAN \
    -DNOMINMAX \
    -mwindows          # 设置 PE 子系统为 WINDOWS，入口为 WinMain，不弹黑窗口

# ── 链接标志 ──────────────────────────────────────────────────────────────────
# -static-libgcc / -static-libstdc++: 静态嵌入 GCC 运行库，无需随包分发 DLL
# UCRT (api-ms-win-crt-*.dll) 在 Windows 10+ 系统中内置，无需额外分发
LDFLAGS = \
    -static-libgcc \
    -static-libstdc++

# ── 系统库 ────────────────────────────────────────────────────────────────────
# -lole32    : CoInitialize, CoCreateInstance
# -luuid     : IID_IUnknown 等基础 COM GUID（WASAPI GUID 由 audio.cpp 中的
#              INITGUID 宏生成，两者以 DECLSPEC_SELECTANY 共存，无冲突）
# -lshell32  : Shell_NotifyIcon
# -lcomctl32 : InitCommonControlsEx, TRACKBAR_CLASS
# -lgdi32    : CreateFont, CreateSolidBrush, CreateDIBSection ...
# -luser32   : CreateWindow, PostMessage ...
LDLIBS = -lole32 -luuid -lshell32 -lcomctl32 -lgdi32 -luser32

# ── 目标 ──────────────────────────────────────────────────────────────────────
TARGET   = BalancedVolume.exe
SRCS     = main.cpp audio.cpp
HDRS     = audio.h
RC_SRC   = resource.rc
RC_OBJ   = resource.o

.PHONY: all clean

all: $(TARGET)

$(RC_OBJ): $(RC_SRC) icon.ico
	$(WINDRES) $(RC_SRC) -o $(RC_OBJ)

$(TARGET): $(SRCS) $(HDRS) $(RC_OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(RC_OBJ) $(LDLIBS)
	@echo "构建完成: $(TARGET)"

clean:
	rm -f $(TARGET) $(RC_OBJ)
