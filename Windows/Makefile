ifeq ($(origin CXX),default)
CXX := x86_64-w64-mingw32-g++
endif
WINDRES ?= x86_64-w64-mingw32-windres
BUILD_DIR ?= ../build/windows/mingw
TARGET ?= $(BUILD_DIR)/DeviceInfoBridgeWindows.exe

CXXFLAGS ?= -std=c++17 -municode -mwindows -Wall -Wextra -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE -DNOMINMAX -static -static-libgcc -static-libstdc++
LDFLAGS ?= -lws2_32 -liphlpapi -lbcrypt

SOURCES := app_config.cpp device_info.cpp http_server.cpp main.cpp
OBJECTS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.cpp=.o)) $(BUILD_DIR)/app.res

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/app.res: app.rc app.manifest resource.h
	@mkdir -p $(BUILD_DIR)
	$(WINDRES) app.rc -O coff -o $@

clean:
	rm -rf $(BUILD_DIR)
