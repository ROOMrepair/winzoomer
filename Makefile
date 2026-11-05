CXX = g++

MKDIR = mkdir -p
RM= rm -f

BUILD_DIR = ./build
TARGET = $(BUILD_DIR)/winzoomer.exe
DEFINE = -DUNICODE -D_UNICODE

DEBUG = 0

#? set 0 to disable freetype
FREETYPE = 1
FREETYPE_LIBPATH = -L./freetype/

#? change this include path
FREETYPE_INCLUDE = -ID:/Sources/lib-Packages/freetype-2.10.0/include/

INCLUDE = -I./glad/include/
LIBS = -lkernel32 -luser32 -lgdi32 -lShcore -lopengl32 -ld3d11 -ldxgi -ld3dcompiler -lole32 -lruntimeobject

ifeq ($(FREETYPE), 1)
    INCLUDE += $(FREETYPE_INCLUDE)
    LDFLAGS += $(FREETYPE_LIBPATH)
    LIBS += -lfreetype
    DEFINE += -DFREETYPE
endif

ifeq ($(DEBUG),1)
  	DEFINE += -DDEBUG
endif

#? seems that this is needed for mingw to find start up wWinMain
LDFLAGS += -municode -mwindows

CXXFLAGS = -Wall -Wextra -Wno-missing-field-initializers $(DEFINE) -municode -mwindows $(INCLUDE) -std=c++17

OBJECT = $(BUILD_DIR)/main.o \
         $(BUILD_DIR)/monitorSelector.o \
         $(BUILD_DIR)/d3dRenderer.o \
         $(BUILD_DIR)/d3dWgcSession.o

all: $(TARGET)

$(BUILD_DIR):
	$(MKDIR) $(BUILD_DIR)

$(OBJECT): | $(BUILD_DIR)

$(TARGET): $(OBJECT)
	$(CXX) -o $@  $^ $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/main.o: main.cpp systemCursor.h cursorRecovery.h monitorSelector.h screenCapture.h useD3d/renderer.h useD3d/wgcDevice.h useD3d/wgcSession.h wgcCapture.h
	$(CXX) $(CXXFLAGS) -c main.cpp -o $@

$(BUILD_DIR)/d3dRenderer.o: useD3d/renderer.cpp useD3d/renderer.h
	$(CXX) $(CXXFLAGS) -c useD3d/renderer.cpp -o $@

$(BUILD_DIR)/d3dWgcSession.o: useD3d/wgcSession.cpp useD3d/wgcSession.h useD3d/wgcDevice.h useD3d/renderer.h wgcCapture.h monitorSelector.h
	$(CXX) $(CXXFLAGS) -c useD3d/wgcSession.cpp -o $@

$(BUILD_DIR)/wgcCapture.o: wgcCapture.cpp wgcCapture.h monitorSelector.h
	$(CXX) $(CXXFLAGS) -c wgcCapture.cpp -o $@

$(BUILD_DIR)/monitorSelector.o: monitorSelector.cpp
	$(CXX) $(CXXFLAGS) -c $^ -o $@

.PHONY: clean

clean:
	$(RM) $(TARGET)
	$(RM) $(OBJECT)
