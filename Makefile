CXX = g++

MKDIR = mkdir -p
RM= rm -f

BUILD_DIR = ./build
TARGET = $(BUILD_DIR)/winzoomer
DEFINE = -DUNICODE -D_UNICODE

#? set 0 to disable freetype
USE_FREETYPE = 1
FREETYPE_LIBPATH = -L./freetype/

#? change this include path
FREETYPE_INCLUDE = -ID:/Sources/lib-Packages/freetype-2.10.0/include/

INCLUDE = -I./glad/include/
LIBS = -lkernel32 -luser32 -lgdi32 -lShcore -lopengl32

ifeq ($(USE_FREETYPE), 1)
    INCLUDE += $(FREETYPE_INCLUDE)
    LDFLAGS += $(FREETYPE_LIBPATH)
    LIBS += -lfreetype
    DEFINE += -DFREETYPE
endif

#? seems that this is needed for mingw to find start up wWinMain
LDFLAGS += -municode -mwindows

CXXFLAGS = -Wall -Wextra -Wno-missing-field-initializers $(DEFINE) -municode -mwindows $(INCLUDE)

OBJECT = $(BUILD_DIR)/main.o $(BUILD_DIR)/glad.o

all: $(TARGET)

$(TARGET): $(OBJECT) 
	$(MKDIR) $(BUILD_DIR)
	$(CXX) -o $@  $^ $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/main.o: main.cpp
	$(CXX) $(CXXFLAGS) -c $^ -o $@

$(BUILD_DIR)/glad.o: ./glad/src/glad.c
	$(CXX) $(CXXFLAGS) -c $^ -o $@

.PHONY: clean
clean:
	$(RM) $(TARGET)
	$(RM) $(OBJECT)
