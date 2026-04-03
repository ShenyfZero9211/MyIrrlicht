# Irrlicht Audio Visualization Makefile
Target := 01.MyHelloWorld
Sources := main.cpp

# 指向 Irrlicht 根目录
IrrlichtHome := ../..
SYSTEM := Win32-gcc
CXX := i686-w64-mingw32-g++
BinPath := ../../bin/$(SYSTEM)

# 编译选项
CPPFLAGS += -I$(IrrlichtHome)/include
ifdef NDEBUG
CXXFLAGS += -O3
else
CXXFLAGS += -g -Wall
endif

# 链接选项
# 注意：在 Windows 下，系统库的顺序很重要，通常放在 -lIrrlicht 之后
LDFLAGS += -L$(IrrlichtHome)/lib/$(SYSTEM) -lIrrlicht -lopengl32 -lgdi32 -luser32 -lwinmm

# 目标文件定义
DESTPATH = $(BinPath)/$(Target).exe

all_win32:
	@if not exist $(subst /,\,$(BinPath)) mkdir $(subst /,\,$(BinPath))
	@echo "Building wasapi_bridge.dll..."
	i686-w64-mingw32-gcc -std=c99 -shared wasapi_bridge.c -o wasapi_bridge.dll -lole32 -luuid -lksuser -lsetupapi
	copy /Y wasapi_bridge.dll $(subst /,\,$(BinPath)\wasapi_bridge.dll)
	copy /Y settings.cfg $(subst /,\,$(BinPath)\settings.cfg)
	@echo "Building $(Target) for Windows..."
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(Sources) -o $(DESTPATH) $(LDFLAGS)

clean:
	@echo "Cleaning..."
	del $(subst /,\,$(DESTPATH))

.PHONY: all_win32 clean
