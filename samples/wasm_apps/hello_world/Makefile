# Hello World WASM App - AkiraOS
# Build for Ocre Runtime (WASI-based)
# Based on OCRE SDK patterns: https://github.com/project-ocre/ocre-sdk
#
# Two build methods available:
# 1. CMake (recommended): ./build.sh
# 2. Direct make: make
#
# For embedded targets with constrained memory, we limit WASM linear memory
# to 64KB (1 page) which fits well in typical WAMR heap configurations.

WASI_SDK ?= /opt/wasi-sdk
CC = $(WASI_SDK)/bin/clang

# Target
TARGET = hello_world.wasm
SRCS = main.c

# Compiler flags - following OCRE SDK patterns
CFLAGS = -O3
CFLAGS += -Wall -Wextra
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-unknown-attributes

# Linker flags - optimized for embedded WAMR targets
LDFLAGS = -Wl,--strip-all
LDFLAGS += -Wl,--allow-undefined
# Memory constraints for embedded targets
LDFLAGS += -z stack-size=4096
LDFLAGS += -Wl,--initial-memory=65536
LDFLAGS += -Wl,--max-memory=65536

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "Built: $(TARGET)"
	@ls -la $(TARGET)

# Build using CMake (recommended for consistency with OCRE SDK)
cmake-build:
	@mkdir -p build
	@cd build && cmake .. && make
	@cp build/hello-world.wasm $(TARGET)
	@echo "Built via CMake: $(TARGET)"

info: $(TARGET)
	@echo "=== WASM Module Info ==="
	@ls -la $(TARGET)
	@echo ""
	@hexdump -C $(TARGET) | head -20

clean:
	rm -f $(TARGET)
	rm -rf build

.PHONY: all clean info cmake-build
