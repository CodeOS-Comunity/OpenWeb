CMAKE ?= cmake
BUILD_DIR ?= build

.PHONY: all clean run

all: $(BUILD_DIR)/openweb

$(BUILD_DIR)/openweb: CMakeLists.txt src/openweb.c src/litehtml_renderer.cpp src/litehtml_renderer.h
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR) --parallel

run: all
	./$(BUILD_DIR)/openweb

clean:
	rm -rf $(BUILD_DIR)
