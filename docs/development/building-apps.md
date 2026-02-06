# Building WASM Applications

Complete guide to developing WebAssembly applications for AkiraOS.

## Toolchain Setup

### WASI SDK (Recommended)

```bash
cd ~
wget https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-21/wasi-sdk-21.0-linux.tar.gz
tar xvf wasi-sdk-21.0-linux.tar.gz
export WASI_SDK_PATH=~/wasi-sdk-21.0
```

## Build Process

### Simple App

```c
// app.c
#include <stdint.h>

__attribute__((import_module("akira")))
__attribute__((import_name("log")))
extern void akira_log(const char *msg, uint32_t len);

__attribute__((export_name("_start")))
void app_main() {
    akira_log("Hello from WASM!", 17);
}
```

**Compile:**
```bash
$WASI_SDK_PATH/bin/clang \
  --target=wasm32-wasi \
  --sysroot=$WASI_SDK_PATH/share/wasi-sysroot \
  -O3 \
  -Wl,--no-entry \
  -Wl,--export=_start \
  -Wl,--allow-undefined \
  -o app.wasm \
  app.c
```

## Optimization

### Size Optimization

```bash
# Use wasm-opt
wasm-opt -Oz app.wasm -o app_optimized.wasm

# Strip debug info
wasm-strip app.wasm
```

## Deployment

### Upload via HTTP

```bash
curl -X POST -F "file=@app.wasm" http://192.168.1.100/upload
```

## Related Documentation

- [First App Tutorial](../getting-started/first-app.md)
- [Native API Reference](../api-reference/native-api.md)
- [Manifest Format](../api-reference/manifest-format.md)
