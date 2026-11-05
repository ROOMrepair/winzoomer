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
LIBS = -lkernel32 -luser32 -lgdi32 -lShcore -lopengl32 -ld3d11 -lole32 -lruntimeobject

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
				 $(BUILD_DIR)/glad.o \
				 $(BUILD_DIR)/simpleText.o \
				 $(BUILD_DIR)/simpleShader.o \
				 $(BUILD_DIR)/monitorSelector.o \
				 $(BUILD_DIR)/wgcCapture.o


all: $(TARGET)

$(BUILD_DIR):
	$(MKDIR) $(BUILD_DIR)

$(OBJECT): | $(BUILD_DIR)

$(TARGET): $(OBJECT)
	$(CXX) -o $@  $^ $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/main.o: main.cpp systemCursor.h cursorRecovery.h monitorSelector.h screenCapture.h wgcCapture.h
	$(CXX) $(CXXFLAGS) -c main.cpp -o $@

$(BUILD_DIR)/wgcCapture.o: wgcCapture.cpp wgcCapture.h monitorSelector.h
	$(CXX) $(CXXFLAGS) -c wgcCapture.cpp -o $@

$(BUILD_DIR)/simpleText.o: simpleText.cpp
	$(CXX) $(CXXFLAGS) -c $^ -o $@

$(BUILD_DIR)/simpleShader.o: simpleShader.cpp
	$(CXX) $(CXXFLAGS) -c $^ -o $@

$(BUILD_DIR)/monitorSelector.o: monitorSelector.cpp
	$(CXX) $(CXXFLAGS) -c $^ -o $@

$(BUILD_DIR)/glad.o: ./glad/src/glad.c
	$(CXX) $(CXXFLAGS) -c $^ -o $@

.PHONY: clean test
test: $(BUILD_DIR)/realtime_tests.exe $(BUILD_DIR)/cursor_visibility_tests.exe $(BUILD_DIR)/cursor_watchdog_tests.exe
	$(BUILD_DIR)/realtime_tests.exe
	$(BUILD_DIR)/cursor_visibility_tests.exe
	$(BUILD_DIR)/cursor_watchdog_tests.exe

$(BUILD_DIR)/cursor_watchdog_tests.exe: tests/cursor_watchdog_tests.cpp cursorRecovery.h systemCursor.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ tests/cursor_watchdog_tests.cpp -luser32 -mconsole

$(BUILD_DIR)/cursor_visibility_tests.exe: tests/cursor_visibility_tests.cpp systemCursor.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ tests/cursor_visibility_tests.cpp -luser32 -mconsole

$(BUILD_DIR)/realtime_tests.exe: tests/realtime_tests.cpp main.cpp systemCursor.h cursorRecovery.h screenCapture.h wgcCapture.h $(filter-out $(BUILD_DIR)/main.o,$(OBJECT))
	$(CXX) $(CXXFLAGS) -o $@ tests/realtime_tests.cpp $(filter %.o,$^) $(LDFLAGS) $(LIBS) -mconsole

clean:
	$(RM) $(TARGET)
	$(RM) $(OBJECT)
