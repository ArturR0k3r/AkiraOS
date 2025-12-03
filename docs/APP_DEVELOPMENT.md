# AkiraOS App Development Guide

This guide explains how to develop, compile, and deploy WebAssembly (WASM) applications for AkiraOS.

## Table of Contents

1. [Overview](#overview)
2. [Prerequisites](#prerequisites)
3. [Toolchain Setup](#toolchain-setup)
4. [Creating Your First App](#creating-your-first-app)
5. [AkiraOS APIs](#akiraos-apis)
6. [Building WASM Apps](#building-wasm-apps)
7. [App Manifest](#app-manifest)
8. [Deploying Apps](#deploying-apps)
9. [Debugging](#debugging)
10. [Best Practices](#best-practices)

---

## Overview

AkiraOS runs WebAssembly applications through the **OCRE (Open Container Runtime Environment)** powered by **WAMR (WebAssembly Micro Runtime)**. This allows apps written in C, C++, Rust, or any language that compiles to WASM to run on embedded devices.

### Architecture

```
┌─────────────────────────────────┐
│     Your WASM App (.wasm)       │  ← Games, Tools, Utilities
├─────────────────────────────────┤
│     OCRE Runtime                │  ← Security, Sandboxing
├─────────────────────────────────┤
│     WAMR Interpreter            │  ← WASM Execution
├─────────────────────────────────┤
│     AkiraOS / Zephyr RTOS       │  ← System Services
├─────────────────────────────────┤
│     Hardware (ESP32-S3, etc.)   │
└─────────────────────────────────┘
```

### Key Features

- **Sandboxed Execution**: Apps run in isolated containers
- **Portable**: Same WASM binary runs on all AkiraOS devices
- **Multiple Languages**: C, C++, Rust, AssemblyScript, etc.
- **Rich APIs**: Access sensors, GPIO, display, timers, networking
- **Hot Deploy**: Install/update apps without reflashing firmware

---

## Prerequisites

### Required Software

1. **WASI SDK** (WebAssembly System Interface SDK)
   - Provides clang/llvm toolchain for WASM compilation
   - Download: https://github.com/WebAssembly/wasi-sdk/releases

2. **Python 3.8+** (for build scripts)

3. **Optional: Rust with wasm32-wasi target**
   ```bash
   rustup target add wasm32-wasi
   ```

4. **Optional: Emscripten** (alternative toolchain)
   ```bash
   git clone https://github.com/emscripten-core/emsdk.git
   cd emsdk && ./emsdk install latest && ./emsdk activate latest
   ```

---

## Toolchain Setup

### Installing WASI SDK (Recommended)

```bash
# Download WASI SDK (check for latest version)
WASI_VERSION=24
WASI_VERSION_FULL=${WASI_VERSION}.0
wget https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_VERSION}/wasi-sdk-${WASI_VERSION_FULL}-x86_64-linux.tar.gz

# Extract to /opt (or your preferred location)
sudo tar xvf wasi-sdk-${WASI_VERSION_FULL}-x86_64-linux.tar.gz -C /opt
sudo ln -sf /opt/wasi-sdk-${WASI_VERSION_FULL} /opt/wasi-sdk

# Add to PATH
echo 'export WASI_SDK_PATH=/opt/wasi-sdk' >> ~/.bashrc
echo 'export PATH=$WASI_SDK_PATH/bin:$PATH' >> ~/.bashrc
source ~/.bashrc

# Verify installation
$WASI_SDK_PATH/bin/clang --version
```

### Alternative: Using Docker

```bash
# Use pre-built WASI SDK Docker image
docker pull piotrekgie/wasi-sdk:latest

# Compile with Docker
docker run --rm -v $(pwd):/src piotrekgie/wasi-sdk \
    /opt/wasi-sdk/bin/clang -O3 -nostdlib \
    -Wl,--no-entry -Wl,--export-all \
    -o /src/app.wasm /src/app.c
```

---

## Creating Your First App

### Hello World Example

Create `hello_world.c`:

```c
/**
 * @file hello_world.c
 * @brief Simple Hello World app for AkiraOS
 */

#include <stdint.h>

/* Import OCRE sleep function */
__attribute__((import_module("env")))
__attribute__((import_name("ocre_sleep")))
extern int ocre_sleep(int milliseconds);

/* Export main entry point */
__attribute__((export_name("_start")))
void _start(void) {
    /* App runs in a loop */
    while (1) {
        /* Sleep for 1 second */
        ocre_sleep(1000);
    }
}
```

### Compile

```bash
# Using WASI SDK
$WASI_SDK_PATH/bin/clang \
    -O3 \
    -nostdlib \
    -Wl,--no-entry \
    -Wl,--export=_start \
    -Wl,--allow-undefined \
    -o hello_world.wasm \
    hello_world.c

# Check the output
file hello_world.wasm
wasm-objdump -x hello_world.wasm | head -50
```

---

## AkiraOS APIs

AkiraOS exposes several native functions to WASM apps through OCRE.

### Core APIs

| Function | Signature | Description |
|----------|-----------|-------------|
| `ocre_sleep(ms)` | `(i)i` | Sleep for milliseconds |
| `uname(buf)` | `(*)i` | Get system information |

### Timer APIs (CONFIG_OCRE_TIMER)

```c
/* Timer callback registration */
int ocre_register_dispatcher(int type, const char *function_name);

/* Get timer events */
int ocre_get_event(int timeout_ms, int *type, int *id, 
                   int *param1, int *param2, int *param3);
```

### Sensor APIs (CONFIG_OCRE_SENSORS)

```c
/* Initialize sensor subsystem */
int ocre_sensors_init(void);

/* Discover available sensors */
int ocre_sensors_discover(void);

/* Open sensor by index */
int ocre_sensors_open(int index);

/* Open sensor by name */
int ocre_sensors_open_by_name(const char *name);

/* Get sensor handle */
int ocre_sensors_get_handle(int index);

/* Get channel count */
int ocre_sensors_get_channel_count(int handle);

/* Read sensor value */
float ocre_sensors_read(int handle, int channel);
```

### GPIO APIs (CONFIG_OCRE_GPIO)

```c
/* Configure GPIO pin */
int ocre_gpio_configure(int port, int pin, int flags);

/* Set GPIO output */
int ocre_gpio_set(int port, int pin, int value);

/* Get GPIO input */
int ocre_gpio_get(int port, int pin);
```

### Messaging APIs (CONFIG_OCRE_CONTAINER_MESSAGING)

```c
/* Publish a message */
int ocre_publish_message(const char *topic, const char *content_type, 
                         void *payload, int payload_len);

/* Subscribe to messages */
int ocre_subscribe_message(const char *topic);
```

### Header File

Create `akira_api.h` for your apps:

```c
/**
 * @file akira_api.h
 * @brief AkiraOS WASM API declarations
 */

#ifndef AKIRA_API_H
#define AKIRA_API_H

#include <stdint.h>

/* Attribute macros for WASM imports */
#define WASM_IMPORT(name) \
    __attribute__((import_module("env"))) \
    __attribute__((import_name(#name)))

#define WASM_EXPORT(name) \
    __attribute__((export_name(#name)))

/* Core APIs */
WASM_IMPORT(ocre_sleep)
extern int ocre_sleep(int milliseconds);

WASM_IMPORT(uname)
extern int uname(void *buf);

/* Sensor APIs */
WASM_IMPORT(ocre_sensors_init)
extern int ocre_sensors_init(void);

WASM_IMPORT(ocre_sensors_discover)
extern int ocre_sensors_discover(void);

WASM_IMPORT(ocre_sensors_open)
extern int ocre_sensors_open(int index);

WASM_IMPORT(ocre_sensors_read)
extern float ocre_sensors_read(int handle, int channel);

/* Timer/Event APIs */
WASM_IMPORT(ocre_register_dispatcher)
extern int ocre_register_dispatcher(int type, const char *function_name);

WASM_IMPORT(ocre_get_event)
extern int ocre_get_event(int timeout, int *type, int *id,
                          int *p1, int *p2, int *p3);

/* Resource types for events */
#define OCRE_RESOURCE_TIMER    0
#define OCRE_RESOURCE_GPIO     1
#define OCRE_RESOURCE_SENSOR   2
#define OCRE_RESOURCE_MESSAGE  3

#endif /* AKIRA_API_H */
```

---

## Building WASM Apps

### Using the Build Script

AkiraOS provides `scripts/build_wasm_app.sh`:

```bash
# Build a single file
./scripts/build_wasm_app.sh my_app.c

# Build with optimization
./scripts/build_wasm_app.sh -O3 my_app.c

# Build multiple source files
./scripts/build_wasm_app.sh -o my_app.wasm src1.c src2.c
```

### Using Makefile

Example `Makefile` for WASM apps:

```makefile
# AkiraOS WASM App Makefile

WASI_SDK ?= /opt/wasi-sdk
CC = $(WASI_SDK)/bin/clang
OBJDUMP = $(WASI_SDK)/bin/wasm-objdump

# Compiler flags
CFLAGS = -O3 -nostdlib
CFLAGS += -I./include

# Linker flags
LDFLAGS = -Wl,--no-entry
LDFLAGS += -Wl,--export=_start
LDFLAGS += -Wl,--allow-undefined
LDFLAGS += -Wl,--strip-all

# Source files
SRCS = main.c
OBJS = $(SRCS:.c=.o)
TARGET = app.wasm

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

info: $(TARGET)
	$(OBJDUMP) -x $(TARGET) | head -100

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean info
```

### Compiler Flags Reference

| Flag | Description |
|------|-------------|
| `-O3` | Maximum optimization |
| `-Os` | Optimize for size (recommended for embedded) |
| `-nostdlib` | Don't link standard library |
| `-Wl,--no-entry` | No `main()` required |
| `-Wl,--export=<sym>` | Export symbol to host |
| `-Wl,--export-all` | Export all symbols |
| `-Wl,--allow-undefined` | Allow imports from host |
| `-Wl,--strip-all` | Remove debug info (smaller binary) |

### Building with Rust

```rust
// Cargo.toml
[package]
name = "akira_app"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

[profile.release]
opt-level = "s"
lto = true
```

```rust
// src/lib.rs
extern "C" {
    fn ocre_sleep(ms: i32) -> i32;
}

#[no_mangle]
pub extern "C" fn _start() {
    loop {
        unsafe { ocre_sleep(1000); }
    }
}
```

```bash
# Build
cargo build --release --target wasm32-wasi
cp target/wasm32-wasi/release/akira_app.wasm ./app.wasm
```

---

## App Manifest

Apps can include an optional JSON manifest for metadata and configuration.

### manifest.json

```json
{
    "name": "my_app",
    "version": "1.0.0",
    "description": "My awesome AkiraOS app",
    "author": "Developer Name",
    "entry": "_start",
    "heap_kb": 16,
    "stack_kb": 4,
    "permissions": ["sensor", "timer", "display"],
    "restart": {
        "enabled": true,
        "max_retries": 3,
        "delay_ms": 1000
    }
}
```

### Manifest Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | string | filename | App identifier |
| `version` | string | "1.0.0" | Semantic version |
| `description` | string | "" | Human-readable description |
| `entry` | string | "_start" | Entry function name |
| `heap_kb` | int | 16 | WASM heap size in KB |
| `stack_kb` | int | 4 | WASM stack size in KB |
| `permissions` | array | [] | Required permissions |
| `restart.enabled` | bool | true | Auto-restart on crash |
| `restart.max_retries` | int | 3 | Max restart attempts |
| `restart.delay_ms` | int | 1000 | Delay between restarts |

### Available Permissions

- `gpio` - GPIO access
- `i2c` - I2C bus access  
- `spi` - SPI bus access
- `sensor` - Sensor readings
- `display` - Display output
- `storage` - File system access
- `network` - Network sockets
- `ble` - Bluetooth LE
- `rf` - RF subsystem

---

## Deploying Apps

### Via HTTP (Web Interface)

1. Connect to AkiraOS WiFi or same network
2. Open browser to `http://<device-ip>/`
3. Navigate to "Apps" section
4. Click "Upload App" and select `.wasm` file
5. App will be installed and started

### Via HTTP (curl)

```bash
# Install app
curl -X POST -F "app=@my_app.wasm" \
     http://<device-ip>/api/apps/install

# List installed apps
curl http://<device-ip>/api/apps/list

# Start app
curl -X POST http://<device-ip>/api/apps/start?name=my_app

# Stop app
curl -X POST http://<device-ip>/api/apps/stop?name=my_app

# Uninstall app
curl -X DELETE http://<device-ip>/api/apps/uninstall?name=my_app
```

### Via Bluetooth LE

Use the AkiraOS companion app or custom BLE client:

1. Scan for device advertising "AkiraOS"
2. Connect to App Transfer service (UUID: `414b4952-xxxx-xxxx-xxxx-xxxxxxxxxxxx`)
3. Write app name to Name characteristic
4. Write app data chunks to Data characteristic (max 512 bytes each)
5. Write to Control characteristic to finalize

### Via SD Card

1. Copy `.wasm` files to SD card root or `/apps/` directory
2. Insert SD card into device
3. Use shell command: `app scan`
4. Apps are discovered and can be installed

### Via Shell Commands

```bash
# List installed apps
akira> app list

# Show app details
akira> app info my_app

# Start an app
akira> app start my_app

# Stop an app
akira> app stop my_app

# Restart an app  
akira> app restart my_app

# Uninstall an app
akira> app uninstall my_app

# Scan for apps on SD card
akira> app scan
```

---

## Debugging

### Printf Debugging

WASM apps can use the host's printf through WASI:

```c
#include <stdio.h>

void debug_print(const char *msg) {
    printf("[APP] %s\n", msg);
}
```

Note: Requires WASI libc support. For minimal builds, use:

```c
/* Import host print function if available */
WASM_IMPORT(debug_print)
extern void debug_print(const char *msg, int len);
```

### Shell Debugging

```bash
# View system logs (includes app output)
akira> debug log

# Check app state
akira> app info my_app

# View memory usage
akira> system info
```

### Common Issues

| Problem | Cause | Solution |
|---------|-------|----------|
| "Import not found" | Missing host function | Check API spelling, enable feature in prj.conf |
| "Out of memory" | Heap too small | Increase `heap_kb` in manifest |
| "Stack overflow" | Deep recursion | Increase `stack_kb` or optimize code |
| App crashes immediately | Entry not exported | Add `-Wl,--export=_start` |
| App too large | Binary size limit | Use `-Os`, strip debug info |

---

## Best Practices

### Memory Management

1. **Use static allocation when possible**
   ```c
   static uint8_t buffer[1024];  /* Better than malloc */
   ```

2. **Minimize heap usage** - Default heap is 16KB

3. **Avoid deep recursion** - Limited stack space

### Performance

1. **Use integer math** when floating-point isn't needed
2. **Batch sensor reads** rather than polling
3. **Use event-driven design** with `ocre_get_event()`
4. **Sleep when idle** to save power

### Code Size

1. **Use `-Os` optimization** for smaller binaries
2. **Strip debug symbols**: `-Wl,--strip-all`
3. **Avoid large libraries** - Write minimal code
4. **Share code** between apps using modules

### Security

1. **Request only needed permissions**
2. **Validate all inputs**
3. **Don't store secrets in WASM** - Use secure storage APIs
4. **Sign your apps** for production deployment

---

## Example Apps

See `samples/wasm_apps/` for complete examples:

- `hello_world/` - Basic app structure
- `sensor_demo/` - Reading sensors  
- `blink_led/` - GPIO control
- `timer_demo/` - Using timers and events

---

## Resources

- [OCRE Project](https://github.com/project-ocre/ocre-runtime)
- [WAMR Documentation](https://github.com/bytecodealliance/wasm-micro-runtime)
- [WASI SDK](https://github.com/WebAssembly/wasi-sdk)
- [WebAssembly Specification](https://webassembly.org/)

---

## Troubleshooting

### Build Errors

**Error: "wasm-ld: error: unknown argument"**
- Update WASI SDK to latest version

**Error: "undefined symbol: __wasm_call_ctors"**
- Add `-nostartfiles` or implement `__wasm_call_ctors`

**Error: "file too large"**
- Reduce code size, check CONFIG_AKIRA_APP_MAX_SIZE_KB

### Runtime Errors

**"Failed to load app"**
- Check WASM magic number (should be `\0asm`)
- Verify exports with `wasm-objdump -x app.wasm`

**"Import resolution failed"**
- Function name mismatch
- Feature not enabled in firmware

**App hangs**
- Add sleep calls in loops
- Check for infinite loops
- Use watchdog timeout

---

*For more help, open an issue on GitHub or join the AkiraOS community Discord.*
