CXX = g++
CXXFLAGS = -Wall -Wextra $(DEFINE) -municode -mwindows $(INCLUDE)
INCLUDE = -I./glad/include/

# need this for mingw to find start up wWinMain
LDFLAGS = -municode -mwindows

LIBS = -lkernel32 -luser32 -lgdi32 -lShcore -lopengl32

DEFINE = -DUNICODE -D_UNICODE -DDEBUG

BUILD_DIR = ./build

MKDIR = mkdir -p
RM= rm -f

TARGET = $(BUILD_DIR)/colorpicker

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
