# Hello World WASM App
# Build for WAMR libc-builtin (NOT WASI)
# ESP32/Xtensa uses libc-builtin mode which provides env.printf, env.puts, etc.

WASI_SDK ?= /opt/wasi-sdk
CC = $(WASI_SDK)/bin/clang

# Compiler flags - target wasm32-unknown-unknown (NOT wasi)
# This produces imports from "env" module compatible with libc-builtin
CFLAGS = --target=wasm32-unknown-unknown
CFLAGS += -O3 -nostdlib
CFLAGS += -Wall -Wextra
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-unknown-attributes
CFLAGS += -Wl,--no-entry
CFLAGS += -Wl,--export=main
CFLAGS += -Wl,--export=__main_argc_argv
CFLAGS += -Wl,--allow-undefined
CFLAGS += -Wl,--strip-all

# Target
TARGET = hello_world.wasm
SRCS = main.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^
	@echo "Built: $(TARGET)"
	@ls -la $(TARGET)

info: $(TARGET)
	@echo "=== WASM Module Info ==="
	@hexdump -C $(TARGET) | head -30

clean:
	rm -f $(TARGET)

.PHONY: all clean info
