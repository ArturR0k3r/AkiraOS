# Hello World WASM Application
# Minimal example for AkiraOS WAMR runtime
# Uses libc-builtin mode with env module (NOT WASI)

WASI_SDK ?= /opt/wasi-sdk
CC = $(WASI_SDK)/bin/clang
OBJDUMP = $(WASI_SDK)/bin/wasm-objdump

# Target
TARGET = hello_world.wasm
SRCS = main.c

# Compiler flags for embedded WAMR
# -O3: Optimize for performance (hello_world is simple enough for -O3)
# -nostdlib: Avoid WASI imports, only use env module
# -Wall -Wextra: Enable warnings for quality
CFLAGS = -O3 -nostdlib
CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wno-unknown-attributes

# Linker flags for WAMR embedded mode
# --no-entry: No default _start entry point
# --export=main: Export main function (OCRE runtime entry point)
# --allow-undefined: Allow imports from env module (putchar, etc.)
# --strip-all: Remove all symbols for minimal size (382 bytes)
# -z stack-size: Set stack to 4KB (leaving 60KB for heap)
# --initial-memory/--max-memory: Limit to 64KB (one WebAssembly page)
LDFLAGS = -Wl,--no-entry
LDFLAGS += -Wl,--export=main
LDFLAGS += -Wl,--allow-undefined
LDFLAGS += -Wl,--strip-all
LDFLAGS += -z stack-size=4096
LDFLAGS += -Wl,--initial-memory=65536
LDFLAGS += -Wl,--max-memory=65536

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "✓ Built: $(TARGET)"
	@ls -la $(TARGET)

info: $(TARGET)
	@echo "=== WASM Module Info ==="
	$(OBJDUMP) -x $(TARGET) | head -60

clean:
	rm -f $(TARGET)
	rm -rf build

.PHONY: all clean info
