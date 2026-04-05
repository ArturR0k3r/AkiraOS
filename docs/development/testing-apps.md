---
layout: default
title: Testing Apps with the Simulator
parent: Development
nav_order: 5
permalink: /development/testing-apps
---

# Testing WASM Apps with the Simulator

The AkiraConsole Simulator (`akira_sim`) lets you run any AkiraOS WASM app on a
Linux desktop before flashing to hardware.  It uses SDL2 to emulate the
320×240 display, GPIO buttons, and the SD card filesystem.

---

## Prerequisites

| Dependency | Install |
|---|---|
| CMake ≥ 3.20 | `sudo apt install cmake` |
| SDL2 | `sudo apt install libsdl2-dev` |
| WASI SDK | see [Building WASM Apps](building-apps.md) |

---

## 1. Build the Simulator

```bash
cd ~/Akira/AkiraOS
mkdir -p sim_build && cd sim_build
cmake ../sim
cmake --build . -j$(nproc)
```

The output binary is `sim_build/akira_sim`.

---

## 2. Build the Sample Apps

```bash
cd ~/Akira/AkiraSDK/wasm_apps

# Build all samples at once
make WASI_SDK=/opt/wasi-sdk

# Or build a single app
make display_test WASI_SDK=/opt/wasi-sdk
make storage_test WASI_SDK=/opt/wasi-sdk
make hello_world  WASI_SDK=/opt/wasi-sdk
```

Built `.wasm` files land in each app's own directory.

---

## 3. Run an App

```bash
~/Akira/AkiraOS/sim_build/akira_sim  <path/to/app.wasm>
```

**Examples:**

```bash
# Display primitives smoke-test (runs to completion then exits)
~/Akira/AkiraOS/sim_build/akira_sim \
    ~/Akira/AkiraSDK/wasm_apps/display_test/display_test.wasm

# Storage API test (runs to completion then exits)
~/Akira/AkiraOS/sim_build/akira_sim \
    ~/Akira/AkiraSDK/wasm_apps/storage_test/storage_test.wasm

# Hello World (prints to terminal, exits)
~/Akira/AkiraOS/sim_build/akira_sim \
    ~/Akira/AkiraSDK/wasm_apps/hello_world/hello_world.wasm
```

The simulator window opens at **640×480** (2× scale).  Press **F** to toggle
between 1× and 2× scale.

---

## 4. Simulator Controls

| Key | AkiraConsole button |
|---|---|
| `W` / `↑` | D-pad Up |
| `S` / `↓` | D-pad Down |
| `A` / `←` | D-pad Left |
| `D` / `→` | D-pad Right |
| `Z` | A |
| `X` | B |
| `C` | Y |
| `Q` | HOME / Settings |
| `F` | Toggle display scale (1× / 2×) |
| Window close | Exit |

---

## 5. SD Card (Storage API)

The simulator maps `./sdcard/` in the current working directory to the app's
sandboxed storage root.

```bash
# Create the virtual SD card directory before running storage apps
mkdir -p ./sdcard

~/Akira/AkiraOS/sim_build/akira_sim \
    ~/Akira/AkiraSDK/wasm_apps/storage_test/storage_test.wasm
```

Files written by the app appear under `./sdcard/` and persist between runs.

---

## 6. Expected Output per App

### `display_test`

```
[wasm] =====================================
[wasm]   AkiraOS Display Test Application
[wasm] =====================================
[wasm] Test 1: Clear screen with colors
[wasm] Test 2: Drawing rectangles
[wasm] Test 3: Drawing pixels
[wasm] Test 4: Text rendering
[wasm] Test 5: Complex graphics
[wasm] Display test completed successfully!
[wasm] All graphics primitives tested
```

The window shows a sequence of colour fills, rectangles, pixels, and text.
The process exits cleanly (exit code 0) after all tests pass.

### `storage_test`

```
[wasm] Storage test: write, read, append, list, delete
[wasm] All storage tests passed.
```

Requires `./sdcard/` to exist (see §5 above).

### `hello_world`

```
[wasm] Hello from AkiraOS!
```

---

## 7. Automated Smoke Test

Run all display-safe apps and check exit codes:

```bash
#!/bin/bash
set -e
SIM=~/Akira/AkiraOS/sim_build/akira_sim
APPS=~/Akira/AkiraSDK/wasm_apps
mkdir -p ./sdcard

fail=0
for wasm in \
    "$APPS/hello_world/hello_world.wasm" \
    "$APPS/display_test/display_test.wasm" \
    "$APPS/storage_test/storage_test.wasm"
do
    echo -n "Testing $(basename $wasm)... "
    if timeout 15 "$SIM" "$wasm" > /dev/null 2>&1; then
        echo "PASS"
    else
        echo "FAIL (exit $?)"
        fail=1
    fi
done

exit $fail
```

Save as `sim_smoke_test.sh`, `chmod +x`, then run from any directory.

---

## 8. Retro NES Emulator — Multi-Display Verification

The `retro` app adapts to three display form factors automatically.  The
simulator runs at 320×240 (AkiraConsole / Prod size).  To test the smaller
display paths, pass the `--display-size` flag:

> **Note:** multi-display sim flag support requires `sim/main.c` to accept
> `--display-size WxH` — currently the simulator always renders at 320×240.
> Use physical hardware for Micro (128×128) and DIY (240×240) verification
> until the flag is added.

Expected behaviour per display:

| Configuration | Render mode | What you see |
|---|---|---|
| 320×240 (AkiraConsole / Prod) | `LETTERBOX` — NES frame centred, black bars left/right | Full 256×224 NES image centred horizontally |
| 240×240 (DIY 2.4") | `CROP` — centre 240 columns of NES frame | No side bars, NES rows vertically centred |
| 128×128 (Micro 1.4") | `SCALE2` — 2:1 nearest-neighbour scale to 128×112 | Scaled-down image, vertically centred with 8px black bars |

---

## 9. Troubleshooting

### Simulator crashes with `ASSERTION FAILED: src != NULL`

**Cause:** Stale `sim_build/` against an older WAMR source.

**Fix:**
```bash
rm -rf ~/Akira/AkiraOS/sim_build
mkdir ~/Akira/AkiraOS/sim_build && cd ~/Akira/AkiraOS/sim_build
cmake ../sim && cmake --build . -j$(nproc)
```

### `SDL_Init failed: No available video device`

Running headless (SSH, CI).  Use a virtual framebuffer:

```bash
sudo apt install xvfb
Xvfb :99 &
export DISPLAY=:99
~/Akira/AkiraOS/sim_build/akira_sim my_app.wasm
```

### App prints `[sim] WASM: no 'main' or '_start' export found`

The WASM binary was not compiled with `-Wl,--export=main`.  Rebuild the app:

```bash
make <app_name> WASI_SDK=/opt/wasi-sdk
```

### `storage_test` exits with `WASM exception: env.storage_open`

`./sdcard/` directory does not exist.  Run `mkdir -p ./sdcard` first.

### `fatal error: 'akira_api.h' file not found` during build

Check the include path.  The header lives in `AkiraSDK/include/`.  Your
`-I` flag must point there directly, **not** to a parent directory:

```bash
# Correct
-I/path/to/AkiraSDK/include

# Wrong (causes 'include/akira_api.h' not found)
-I/path/to/AkiraSDK
```

The AkiraSDK Makefile handles this automatically via `-I../../include`.
