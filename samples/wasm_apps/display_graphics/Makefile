# Display Graphics Demo Makefile
APP_NAME = display_graphics
WASM_FILE = $(APP_NAME).wasm
SOURCE_FILES = main.c

# WASI SDK paths
WASI_SDK_PATH ?= /opt/wasi-sdk
CC = $(WASI_SDK_PATH)/bin/clang
SYSROOT = $(WASI_SDK_PATH)/share/wasi-sysroot

# Compiler flags
CFLAGS = -O2 -Wall -Wextra \
         --sysroot=$(SYSROOT) \
         --target=wasm32-wasi \
         -I../include \
         -Wl,--no-entry \
         -Wl,--export=_start \
         -Wl,--allow-undefined \
         -nostartfiles

.PHONY: all clean

all: $(WASM_FILE)

$(WASM_FILE): $(SOURCE_FILES)
	$(CC) $(CFLAGS) -o $@ $^
	@echo "Built $(WASM_FILE)"
	@wasm-objdump -x $(WASM_FILE) | grep "name: _start" || echo "Warning: _start not found"

clean:
	rm -f $(WASM_FILE)
