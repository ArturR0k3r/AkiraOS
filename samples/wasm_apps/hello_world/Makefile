# Hello World WASM App
# Build for WAMR libc-builtin (not WASI)

WASI_SDK ?= /opt/wasi-sdk
CC = $(WASI_SDK)/bin/clang

# Include path for akira_api.h
INCLUDE = -I../include

# Compiler flags - target wasm32 freestanding (no WASI)
CFLAGS = --target=wasm32
CFLAGS += -Os -nostdlib
CFLAGS += $(INCLUDE)
CFLAGS += -Wall -Wextra
CFLAGS += -Wl,--no-entry
CFLAGS += -Wl,--export=main
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
	$(OBJDUMP) -x $(TARGET) | head -60

clean:
	rm -f $(TARGET)

.PHONY: all clean info
