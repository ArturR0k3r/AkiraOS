# WASM Apps Quick Reference Card

## Build Commands

```bash
# Using build script (recommended)
cd samples/wasm_apps/hello_world
../../scripts/build_wasm_app.sh main.c

# Using Make
cd samples/wasm_apps/hello_world
make

# Using CMake (hello_world only)
cd samples/wasm_apps/hello_world
./build.sh
```

## Build Script Usage

```bash
# Help
./build_wasm_app.sh -h

# Basic
./build_wasm_app.sh main.c

# Custom output
./build_wasm_app.sh -o myapp.wasm main.c

# Optimization
./build_wasm_app.sh -Os main.c     # Size (default)
./build_wasm_app.sh -O3 main.c     # Performance
./build_wasm_app.sh -O0 main.c     # Debug

# Includes & Defines
./build_wasm_app.sh -I ./include -D DEBUG main.c

# Custom stack size
./build_wasm_app.sh -m 8 main.c    # 8KB stack

# Verbose
./build_wasm_app.sh -v main.c
```

## Code Template

```c
// main.c - Correct pattern for AkiraOS
#include <stdint.h>

// Declare external env module functions
extern int putchar(int c);

// OR for AkiraOS APIs
#include "akira_api.h"

int main(void) {
    // Your code here
    return 0;
}
```

## Critical Rules

✅ **DO**:
- Use `-nostdlib` flag
- Declare functions as `extern`
- Use `entry: "main"` in manifest
- Keep binary < 64KB
- Use `-Os` for size optimization

❌ **DON'T**:
- Include `<stdio.h>`, `<stdlib.h>`
- Use WASI-specific headers
- Use `_start` as entry point
- Exceed 64KB memory limit
- Use floating point (unless essential)

## Memory Model

```
64KB Linear Memory (one WASM page):
┌─────────────────────────┐
│  Stack (4KB)            │ 64KB - 4KB
├─────────────────────────┤
│  Heap (free space)      │ ~60KB
├─────────────────────────┤
│  Data + BSS             │
└─────────────────────────┘

WAMR Heap (ESP32-S3): 2MB in PSRAM
```

## Manifest Template

```json
{
    "name": "my_app",
    "version": "1.0.0",
    "description": "My application",
    "author": "Your Name",
    "entry": "main",
    "heap_kb": 16,
    "stack_kb": 4,
    "permissions": [],
    "restart": {
        "enabled": true,
        "max_retries": 3,
        "delay_ms": 1000
    }
}
```

## Makefile Template

```makefile
WASI_SDK ?= /opt/wasi-sdk
CC = $(WASI_SDK)/bin/clang

TARGET = app.wasm
SRCS = main.c

CFLAGS = -Os -nostdlib \
         -Wall -Wextra -Wno-unknown-attributes \
         -I../include

LDFLAGS = -Wl,--no-entry -Wl,--export=main \
          -Wl,--allow-undefined -Wl,--strip-all \
          -z stack-size=4096 \
          -Wl,--initial-memory=65536 \
          -Wl,--max-memory=65536

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
```

## Sample Apps

| App | Location | Size | Features |
|-----|----------|------|----------|
| hello_world | `samples/wasm_apps/hello_world/` | ~382B | Minimal, putchar only |
| sensor_demo | `samples/wasm_apps/sensor_demo/` | ~1-2KB | WAMR sensor API |
| blink_led | `samples/wasm_apps/blink_led/` | ~1-2KB | GPIO control |

## Common Issues

| Problem | Solution |
|---------|----------|
| Import function failed | Add `extern` declaration, remove stdio.h |
| Binary too large | Use `-Os`, remove features |
| Memory overflow | Check linker flags, reduce heap |
| Program crash | Check stack usage, validate logic |
| Entry point wrong | Use `entry: "main"` in manifest |

## Documentation

- **Full Standard**: `docs/WASM_BUILD_STANDARD.md`
- **Apps Guide**: `samples/wasm_apps/README.md`
- **Summary**: `WASM_BUILD_STANDARDIZATION_SUMMARY.md`

## Key Flags Summary

| Flag | Purpose | Value |
|------|---------|-------|
| `-Os` | Size optimization | Default |
| `-nostdlib` | Avoid WASI | Required |
| `--export=main` | Entry point | Required |
| `--allow-undefined` | Env imports | Required |
| `--strip-all` | Minimize size | Optional |
| `-z stack-size` | Stack limit | 4096 |
| `--initial-memory` | Start memory | 65536 |
| `--max-memory` | Max memory | 65536 |

## Environment Setup

```bash
# Set WASI SDK path
export WASI_SDK_PATH=/opt/wasi-sdk
export PATH=$PATH:$WASI_SDK_PATH/bin

# Verify
clang --target=wasm32-unknown-wasi --version
```

## Next Steps

1. Build a sample: `cd samples/wasm_apps/hello_world && make`
2. Review code: Check main.c for correct pattern
3. Check manifest: Verify `entry: "main"`
4. Deploy: Copy .wasm to device
5. Test: Run on target hardware
