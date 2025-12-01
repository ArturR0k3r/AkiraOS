# AkiraOS WASM Sample Apps

This directory contains example WebAssembly applications for AkiraOS.

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

## Building

Build all samples:
```bash
make
```

Build a single sample:
```bash
make hello_world
```

Or build manually:
```bash
cd hello_world
make
```

## Samples

### hello_world
Minimal example showing basic app structure. Uses only `ocre_sleep()`.
- **Size**: ~200 bytes
- **Permissions**: none

### sensor_demo  
Demonstrates sensor initialization, discovery, and reading.
- **Size**: ~500 bytes
- **Permissions**: sensor

### blink_led
GPIO control example - configures and toggles an LED.
- **Size**: ~300 bytes
- **Permissions**: gpio

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

## Creating Your Own Apps

1. Copy `hello_world/` as a template
2. Modify `main.c` with your code
3. Update `manifest.json` with your app info
4. Add to `Makefile` SAMPLES list (optional)
5. Build with `make`

See [APP_DEVELOPMENT.md](../../docs/APP_DEVELOPMENT.md) for the full API reference.

## Directory Structure

```
wasm_apps/
├── include/
│   └── akira_api.h      # AkiraOS API header
├── hello_world/
│   ├── main.c           # Source code
│   ├── manifest.json    # App metadata
│   └── Makefile         # Build rules
├── sensor_demo/
│   └── ...
├── blink_led/
│   └── ...
├── Makefile             # Master build file
└── README.md            # This file
```
