# Building WASM Applications

Complete guide to developing WebAssembly applications for AkiraOS.
Apps can be written in **C**, **Rust**, or **Python** (MicroPython).

## Toolchain Setup

### C Apps — WASI SDK

The WASI SDK provides the clang/lld toolchain used to compile `.wasm` binaries. AkiraOS apps target `wasm32-unknown-unknown` — **not** the WASI runtime. No WASI system calls are used at runtime.

```bash
cd ~
wget https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-24/wasi-sdk-24.0-x86_64-linux.tar.gz
tar xvf wasi-sdk-24.0-x86_64-linux.tar.gz
export WASI_SDK_PATH=~/wasi-sdk-24.0
```

### Rust Apps — rustup

```bash
# Install Rust (if not already installed)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
# Add the WASM target
rustup target add wasm32-unknown-unknown
```

No WASI SDK is needed for Rust apps.

### Python Apps — micropython.wasm

Python apps use a prebuilt `micropython.wasm` runtime that includes the `_akira`
native C module. Obtain it from the AkiraOS releases page and place it at:

```
AkiraSDK/python/runtime/micropython.wasm
```

or set the environment variable `MICROPYTHON_WASM=/path/to/micropython.wasm`.

See [AkiraSDK/python/runtime/README.md](../../AkiraSDK/python/runtime/README.md).


## Build Process

### C Apps — Using AkiraSDK (Recommended)

The `AkiraSDK/` submodule includes `include/akira_api.h` and ready-to-build sample apps in `AkiraSDK/wasm_apps/`.

```bash
# Build all C sample apps
cd ~/akira-workspace/AkiraOS/AkiraSDK/wasm_apps
./build.sh

# Build a single C app
./build.sh hello_world
```

### C Apps — Simple Custom App

```c
// app.c
#include "akira_api.h"

int main(void) {
    printf("Hello from WASM!");
    return 0;
}
```

**Compile:**
```bash
$WASI_SDK_PATH/bin/clang \
  -target wasm32-unknown-unknown \
  -nostdlib \
  -fvisibility=hidden \
  -Wl,--no-entry \
  -Wl,--export=main \
  -Wl,--allow-undefined \
  -Wl,--strip-all \
  -z stack-size=4096 \
  -Wl,--initial-memory=65536 \
  -Wl,--max-memory=65536 \
  -I ~/akira-workspace/AkiraOS/AkiraSDK/include \
  -O2 \
  -o app.wasm \
  app.c
```

**Important:** Use `wasm32-unknown-unknown` (bare-metal), not `wasm32-wasi`. AkiraOS provides its own API layer without WASI system calls.

---

## Rust Apps

Full guide: [AkiraSDK/docs/RUST_GUIDE.md](../../AkiraSDK/docs/RUST_GUIDE.md)

### Project structure

```
my_rust_app/
├── Cargo.toml          # crate-type = ["cdylib"]
├── .cargo/config.toml  # target = "wasm32-unknown-unknown" + linker flags
├── manifest.json
└── src/
    └── main.rs
```

A complete template is at `AkiraSDK/wasm_apps/rust/hello_world/`.

### Writing a Rust app

```rust
#![no_std]
#![no_main]

use akira_sdk::{console, display};

#[no_mangle]
pub extern "C" fn main() -> i32 {
    console::println("Hello from Rust WASM!");
    display::clear(display::COLOR_BLACK);
    display::text(10, 10, "Hello Rust!", display::COLOR_WHITE);
    display::flush();
    0
}
```

### Building

```bash
# From the app directory
cargo build --target wasm32-unknown-unknown --release
# Output: target/wasm32-unknown-unknown/release/<name>.wasm

# Via AkiraSDK build script
cd AkiraSDK/wasm_apps
./build.sh rust/my_app

# Via make
cd AkiraSDK/wasm_apps
make rust/my_app
make build-rust    # all Rust apps
```

### Cargo.toml essentials

```toml
[lib]
crate-type = ["cdylib"]   # produces .wasm with C export table
name = "my_app"
path = "src/main.rs"

[dependencies]
akira_sdk = { path = "../../rust/akira_sdk" }

[profile.release]
opt-level = "s"
lto = true
panic = "abort"
strip = true
```

---

## Python Apps (MicroPython)

Full guide: [AkiraSDK/docs/PYTHON_GUIDE.md](../../AkiraSDK/docs/PYTHON_GUIDE.md)

### Architecture

Python scripts are packaged into a WASM binary by injecting the source as a
custom section (`akira_py_script`) into `micropython.wasm`.
At runtime, WAMR loads the binary; MicroPython reads the section and executes
the script. The `_akira` native C module (built into the runtime) maps all
AkiraOS API calls.

```
main.py + micropython.wasm  →  py_to_wasm.py  →  app.wasm
```

### Project structure

```
my_python_app/
├── main.py          # import akira; call API
└── manifest.json    # memory_quota must be ≥ 262144 (256 KB)
```

A complete template is at `AkiraSDK/python/apps/hello_world/`.

### Writing a Python app

```python
import akira

def main():
    akira.print("Hello from Python WASM!")
    akira.display_clear(akira.COLOR_BLACK)
    akira.display_text(10, 10, "Hello Python!", akira.COLOR_WHITE)
    akira.display_flush()

main()
```

### Building

```bash
# Direct (from app directory)
python3 AkiraSDK/scripts/py_to_wasm.py main.py \
    --manifest manifest.json -o my_app.wasm

# Via AkiraSDK build script
cd AkiraSDK/wasm_apps
./build.sh python/my_app

# Via make
cd AkiraSDK/wasm_apps
make python/my_app
make build-python    # all Python apps
```

### manifest.json for Python apps

```json
{
  "name": "my_app",
  "version": "1.0.0",
  "capabilities": ["input.read"],
  "memory_quota": 262144
}
```

> Python apps need **256 KB** (`262144`) for the MicroPython heap.

---

### Manifest File

Create `manifest.json` alongside your app:

```json
{
  "name": "my_app",
  "version": "1.0.0",
  "capabilities": ["display.write"],
  "memory_quota": 65536
}
```

**Embedding Manifest:**

The AkiraSDK build script automatically embeds the manifest as a custom section:

```bash
# Done automatically by build.sh if manifest.json exists
python3 AkiraSDK/scripts/embed_manifest.py app.wasm manifest.json app.wasm
```

This allows the runtime to extract capabilities without requiring a separate JSON file.

### Build Output

```
AkiraSDK/wasm_apps/bin/
├── app.wasm              # WASM bytecode (2-10 KB, universal)
├── app.json              # Standalone manifest
├── app-xtensa.aot        # AOT for ESP32-S3 (10-50 KB)
├── app-thumb.aot         # AOT for nRF54L15
├── app-riscv32.aot       # AOT for ESP32-C3
└── app-x86_64.aot        # AOT for native_sim
```

## Memory Configuration

WASM apps have two separate stack limits:

**1. WASM Linear Memory Stack:** 4KB (set by `-z stack-size=4096`)
- Used for WASM local variables and call frames
- Overflow causes WASM trap

**2. Zephyr Thread Stack:** 8KB (`CONFIG_AKIRA_WASM_APP_STACK_SIZE=8192`)
- Used for native API calls (C call chain)
- Handles host function execution

**Heap Memory:**
- Initial/max: 64KB (65536 bytes)
- Cannot grow beyond limit without rebuilding

**Custom Memory Sizes:**
```bash
clang \
  -z stack-size=8192 \
  -Wl,--initial-memory=131072 \
  -Wl,--max-memory=131072 \
  ...
```

## Optimization

### Optimization Levels

```bash
# Balanced (default in build.sh)
clang -O2 -Wl,--strip-all ...

# Size-optimized (smallest binary)
clang -Oz -Wl,--strip-all ...

# Speed-optimized (faster execution)
clang -O3 ...
```

**AOT Optimization:**
```bash
# Default (balanced)
wamrc --opt-level=3 --size-level=1 -o app.aot app.wasm

# Maximum speed (larger binary)
wamrc --opt-level=3 --size-level=0 -o app.aot app.wasm

# Maximum size reduction  
wamrc --opt-level=0 --size-level=3 -o app.aot app.wasm
```

### AOT Compilation

**For maximum performance**, compile your `.wasm` to native machine code using WAMR's AOT compiler.

#### Setup wamrc

Build `wamrc` from the WAMR submodule already included in AkiraOS (required — the AkiraOS fork includes Xtensa backend support):

```bash
cd ~/akira-workspace/AkiraOS/modules/wasm-micro-runtime/wamr-compiler
cmake . -DWAMR_BUILD_PLATFORM=linux
make
sudo cp wamrc /usr/local/bin/
# or: export WAMRC=$PWD/wamrc
```

#### Compile with the AkiraSDK build script (recommended)

```bash
cd ~/akira-workspace/AkiraOS/AkiraSDK/wasm_apps
./build.sh                    # build .wasm
./build.sh aot                # AOT for ESP32-S3 (xtensa, default)
./build.sh aot thumb          # AOT for nRF54L15 (Cortex-M33)
./build.sh aot thumbv7em      # AOT for STM32 (Cortex-M7)
./build.sh aot riscv32        # AOT for ESP32-C3 (RISC-V)
# Output: bin/<app>-<target>.aot
```

#### Or invoke wamrc directly

```bash
# ESP32-S3 (Xtensa LX7)
wamrc --target=xtensa --cpu=esp32s3 -o app-xtensa.aot app.wasm

# nRF54L15 (Cortex-M33)
wamrc --target=thumb --cpu=cortex-m33 -o app-thumb.aot app.wasm

# ESP32-C3 (RISC-V 32)
wamrc --target=riscv32 -o app-riscv32.aot app.wasm

# Native simulation (x86-64)
wamrc --target=x86_64 -o app-x86_64.aot app.wasm
```

**Performance Gain:** 10-50x faster execution compared to interpreter mode.

**Tradeoff:** Architecture-specific binaries (one `.aot` per platform).

See [AOT Compilation Architecture](../architecture/aot-compilation.md) for detailed guide.

## Deployment

### Upload via HTTP

Apps install through the app-manager route, which streams the **raw** body
(no multipart) — so use `--data-binary`, not `-F`. `/upload` is the firmware
flash endpoint and must not be used for apps.

```bash
# Named app installation (name is optional; defaults to "uploaded_app")
curl -X POST --data-binary @app.wasm \
  "http://192.168.1.100/api/apps/install?name=myapp"

# Hybrid deployment with AOT (best performance): install the .aot the same way
curl -X POST --data-binary @app-xtensa.aot \
  "http://192.168.1.100/api/apps/install?name=myapp"
```

**Limits:** Max file size is `CONFIG_AKIRA_APP_MAX_SIZE_KB` (default: 1MB per app).

**Hybrid Mode:** Runtime automatically uses `.aot` if available for the current architecture, falls back to `.wasm` otherwise.

### SD Card Installation

```bash
# Copy apps to SD card
cp bin/*.wasm /media/SD_CARD/apps/
cp bin/*.json /media/SD_CARD/apps/
# Insert SD → apps auto-scanned on boot
```

### USB Storage Installation

```bash
# Copy to USB drive
cp bin/*.wasm /media/USB_DRIVE/apps/
# Connect USB → scan from shell: app scan usb
```

### Shell Commands

```bash
# View installed apps
AkiraOS:~$ app list

# Install from filesystem
AkiraOS:~$ app install /lfs/apps/myapp.wasm

# Start app
AkiraOS:~$ app start myapp

# View running apps
AkiraOS:~$ app status

# Stop app
AkiraOS:~$ app stop myapp
```

## Related Documentation

- [First App Tutorial](../getting-started/first-app.md)
- [SDK API Reference](../../AkiraSDK/docs/API_REFERENCE.md)
- [Rust App Guide](../../AkiraSDK/docs/RUST_GUIDE.md)
- [Python App Guide](../../AkiraSDK/docs/PYTHON_GUIDE.md)
- [Manifest Format](../api-reference/manifest-format.md)
- [Best Practices](best-practices.md)
- [Advanced Sample Apps](advanced-apps.md)
