CMAKE ?= cmake
BUILD_DIR ?= build
CARGO ?= cargo
RUST_TARGET ?= x86_64-unknown-none

.PHONY: all clean run native native-clean

all: $(BUILD_DIR)/openweb

$(BUILD_DIR)/openweb: CMakeLists.txt src/openweb.c src/litehtml_renderer.cpp src/litehtml_renderer.h
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR) --parallel

run: all
	./$(BUILD_DIR)/openweb

# Native Rust HTTP backend + renderer (no_std static lib for the CodeOS kernel).
native:
	cd native && $(CARGO) build --release --target $(RUST_TARGET)

native-clean:
	cd native && $(CARGO) clean

clean:
	rm -rf $(BUILD_DIR)