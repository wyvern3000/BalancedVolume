CXX     = g++
WINDRES = windres

CXXFLAGS = \
    -std=c++17 -O2 -Wall -Wextra \
    -DUNICODE -D_UNICODE \
    -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
    -mwindows

LDFLAGS = -static-libgcc -static-libstdc++

LDLIBS = -lole32 -luuid -lshell32 -lcomctl32 -lgdi32 -luser32

TARGET  = BalancedVolume.exe
SRCS    = main.cpp audio.cpp
HDRS    = audio.h
RC_SRC  = resource.rc
RC_OBJ  = resource.o

.PHONY: all clean

all: $(TARGET)

$(RC_OBJ): $(RC_SRC) icon.ico
	$(WINDRES) $(RC_SRC) -o $(RC_OBJ)

$(TARGET): $(SRCS) $(HDRS) $(RC_OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(RC_OBJ) $(LDLIBS)
	@echo "构建完成: $(TARGET)"

clean:
	rm -f $(TARGET) $(RC_OBJ)
