# Blink LED WASM App

WASI_SDK ?= /opt/wasi-sdk
CC = $(WASI_SDK)/bin/clang
OBJDUMP = $(WASI_SDK)/bin/wasm-objdump

# Include path
INCLUDE = -I../include

# Compiler flags
CFLAGS = -Os -nostdlib $(INCLUDE)
CFLAGS += -Wall -Wextra

# Linker flags
LDFLAGS = -Wl,--no-entry
LDFLAGS += -Wl,--export=_start
LDFLAGS += -Wl,--allow-undefined
LDFLAGS += -Wl,--strip-all

# Target
TARGET = blink_led.wasm
SRCS = main.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "Built: $(TARGET)"
	@ls -la $(TARGET)

info: $(TARGET)
	@echo "=== WASM Module Info ==="
	$(OBJDUMP) -x $(TARGET) | head -60

clean:
	rm -f $(TARGET)

.PHONY: all clean info
