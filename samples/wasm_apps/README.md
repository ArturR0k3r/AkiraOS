# AkiraOS WASM Applications

This directory contains WebAssembly (WASM) applications for AkiraOS running on the OCRE runtime with WAMR (WebAssembly Micro Runtime) backend.

## Architecture Overview

- **Runtime**: OCRE Container Supervisor (WAMR backend)
- **WAMR Mode**: libc-builtin (NOT WASI) with `env` module imports
- **Memory**: 64KB linear memory per app (one WebAssembly page), backed by PSRAM on ESP32-S3
- **Entry Point**: `main` function (not `_start`)
- **Build System**: WASI SDK + libc-builtin toolchain

## Prerequisites

You need the **WASI SDK** to build these apps. See [APP_DEVELOPMENT.md](../../docs/APP_DEVELOPMENT.md) for installation instructions.

Quick install:
```bash
# Download and install WASI SDK
WASI_VERSION=24
wget https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_VERSION}/wasi-sdk-${WASI_VERSION}.0-x86_64-linux.tar.gz
sudo tar xvf wasi-sdk-${WASI_VERSION}.0-x86_64-linux.tar.gz -C /opt
sudo ln -sf /opt/wasi-sdk-${WASI_VERSION}.0 /opt/wasi-sdk
```

## Building WASM Apps

### Quick Start

**Option 1: Using the build script** (Recommended)
```bash
cd hello_world
../../scripts/build_wasm_app.sh -o hello_world.wasm main.c
```

**Option 2: Using Make**
```bash
cd hello_world
make
```

**Option 3: Using CMake** (hello_world only)
```bash
cd hello_world
./build.sh
```

### Build Script: `build_wasm_app.sh`

Comprehensive build tool for all AkiraOS WASM apps:

```bash
# Basic usage
./build_wasm_app.sh main.c

# With custom output and optimization
./build_wasm_app.sh -o my_app.wasm -O3 main.c

# With includes and defines
./build_wasm_app.sh -I ./include -D DEBUG main.c utils.c

# Custom stack size (8KB instead of 4KB)
./build_wasm_app.sh -m 8 main.c

# Verbose output
./build_wasm_app.sh -v main.c

# Show help
./build_wasm_app.sh -h
```

**Features:**
- Automatic WASI SDK detection
- Configurable optimization levels (-Os, -O0, -O1, -O2, -O3)
- Include path and preprocessor define support
- Memory constraint validation
- Automatic AkiraOS include path handling
- Colored output with progress indicators

## Available Samples

### hello_world

**Description**: Minimal "Hello World" using putchar()

**Files**:
- `main.c`: Direct putchar implementation (no stdio)
- `Makefile`: Simple direct build
- `CMakeLists.txt`: CMake-based build
- `manifest.json`: App metadata

**Build**:
```bash
cd hello_world
make              # Direct make
../../scripts/build_wasm_app.sh main.c  # Using build script
```

**Size**: ~382 bytes (fully stripped)

### sensor_demo

**Description**: Sensor reading example using AkiraOS APIs

**Files**:
- `main.c`: Demonstrates OCRE sensor API usage
- `Makefile`: Standard build with akira_api.h
- `manifest.json`: Sensor permissions configured
- `../include/akira_api.h`: AkiraOS API header

**Build**:
```bash
cd sensor_demo
make
../../scripts/build_wasm_app.sh main.c
```

**Permissions**: Requires `sensor` permission in manifest

### blink_led

**Description**: GPIO/LED control example

**Files**:
- `main.c`: Demonstrates OCRE GPIO API usage
- `Makefile`: Standard build with akira_api.h
- `manifest.json`: GPIO permissions configured
- `../include/akira_api.h`: AkiraOS API header

**Build**:
```bash
cd blink_led
make
../../scripts/build_wasm_app.sh main.c
```

**Permissions**: Requires `gpio` permission in manifest

## Important: Libc-Builtin vs WASI Mode

**AkiraOS WASM apps must run in libc-builtin mode.** This means:

❌ **Do NOT include**:
- `#include <stdio.h>`
- `#include <stdlib.h>`
- `#include <string.h>`
- WASI-specific headers

✅ **DO use**:
- Direct runtime calls (e.g., `putchar()`)
- AkiraOS APIs via `akira_api.h`
- External function declarations for env module

**Why?** The WAMR runtime on ESP32 only provides the `env` module with basic I/O. WASI imports cause undefined reference errors at load time.

### Correct Hello World
```c
// hello_world.c - CORRECT
#include <stdint.h>

// Declare external env module function
extern int putchar(int c);

int main(void) {
    putchar('H');
    putchar('i');
    return 0;
}
```

### Wrong Approach
```c
// hello_world.c - WRONG
#include <stdio.h>  // ❌ This pulls in WASI imports

int main(void) {
    printf("Hi\n");  // ❌ Will fail at load time
    return 0;
}
```

## Compiler Flags Explained

### Standard Compilation
```bash
-Os              # Optimize for size (critical for 64KB limit)
-nostdlib        # NO standard library = NO WASI imports
-Wall -Wextra    # Enable all warnings for quality
-Wno-unknown-attributes  # Accept some OCRE-specific attributes
```

### Standard Linking
```bash
-Wl,--no-entry                      # No default _start entry
-Wl,--export=main                   # Export main as entry point
-Wl,--allow-undefined               # Allow env module imports
-Wl,--strip-all                     # Remove symbols (minimal size)
-z stack-size=4096                  # 4KB stack
-Wl,--initial-memory=65536          # 64KB initial memory
-Wl,--max-memory=65536              # 64KB max (one WASM page)
```

## Manifest Configuration

Each WASM app requires a `manifest.json`:

```json
{
    "name": "my_app",
    "version": "1.0.0",
    "description": "Application description",
    "author": "Your Name",
    "entry": "main",              // Entry function (always "main")
    "heap_kb": 16,                // Heap allocation (in WAMR)
    "stack_kb": 4,                // Stack allocation (in WAMR)
    "permissions": ["sensor"],    // Required permissions
    "restart": {
        "enabled": true,
        "max_retries": 5,
        "delay_ms": 2000
    }
}
```

**Key Fields**:
- `entry`: Must be `"main"` (not `_start`)
- `heap_kb`: Stack/heap space in WAMR (separate from WASM linear memory)
- `stack_kb`: Stack space within WAMR heap
- `permissions`: Required capabilities (sensor, gpio, storage, etc.)

## Typical Application Structure

```
my_app/
├── main.c              # Application code
├── utils.c             # Helper functions
├── Makefile            # Build configuration
├── manifest.json       # App metadata
└── README.md           # Documentation
```

### Minimal main.c
```c
#include <stdint.h>

// Declare external functions
extern int putchar(int c);

int main(void) {
    putchar('H');
    putchar('i');
    putchar('\n');
    return 0;
}
```

### Using AkiraOS APIs
```c
#include <stdint.h>
#include "akira_api.h"  // AkiraOS WAMR API definitions

int main(void) {
    ocre_sensors_init();      // Example OCRE sensor setup
    // Application code
    return 0;
}
```

## Deploying to Device

### Via SD Card
```bash
# With SD card mounted at /media/user/AKIRA
make install SD_MOUNT=/media/user/AKIRA
```

### Via HTTP Upload
```bash
curl -X POST -F "app=@hello_world/hello_world.wasm" http://<device-ip>/api/apps/install
```

### Via Shell
Copy the `.wasm` file to device storage, then:
```
akira> app scan
akira> app start hello_world
```

## Memory Layout

```
Linear Memory (64KB, one WASM page):
┌─────────────────────────────┐
│  Stack (4KB from top)       │  64KB-4KB = 60KB available
├─────────────────────────────┤
│                             │
│  Heap (remaining space)     │  ~60KB for malloc/static data
│                             │
├─────────────────────────────┤
│  Data & BSS                 │
└─────────────────────────────┘
```

## Performance Tips

1. **Use -Os instead of -O0** for best code size
2. **Limit includes** - each header adds code
3. **Inline simple functions** to avoid call overhead
4. **Use uint32_t/uint16_t** instead of int for predictable sizing
5. **Avoid floating point** if possible (significant code bloat)

## Troubleshooting

### "failed to link import function (env, putchar)"
**Cause**: Missing function declaration or wrong header includes
**Fix**: Declare functions as `extern` without including stdio.h

### "region `dram0_0_seg' overflowed"
**Cause**: WASM binary too large or stack size incorrect
**Fix**: Use `-Os`, reduce functionality, check Makefile flags

### "allocate linear memory failed"
**Cause**: WAMR heap too small or PSRAM not configured
**Fix**: Check ESP32-S3 CONFIG_MEMC=y and PSRAM heap size

### Binary too large (>64KB)
**Cause**: Excessive code or data
**Fix**: Reduce functionality, use better compiler flags, check for floating point

## Directory Structure

```
wasm_apps/
├── include/
│   └── akira_api.h      # AkiraOS API header
├── scripts/
│   └── build_wasm_app.sh # Common build script
├── hello_world/
│   ├── main.c           # Source code
│   ├── Makefile         # Build rules
│   ├── manifest.json    # App metadata
│   └── CMakeLists.txt   # CMake build (optional)
├── sensor_demo/
│   ├── main.c
│   ├── Makefile
│   └── manifest.json
├── blink_led/
│   ├── main.c
│   ├── Makefile
│   └── manifest.json
└── README.md            # This file
```

## References

- **OCRE SDK**: https://github.com/project-ocre/ocre-sdk
- **WAMR**: https://github.com/bytecodealliance/wasm-micro-runtime
- **WASI**: https://wasi.dev
- **WebAssembly**: https://webassembly.org
