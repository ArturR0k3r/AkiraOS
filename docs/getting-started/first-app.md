---
layout: default
title: First App
parent: Getting Started
nav_order: 2
---

# Building Your First WASM App

Create, build, and deploy a "Hello World" WebAssembly application on AkiraOS.

## Prerequisites

- AkiraOS firmware flashed and running on a device (or `native_sim` built)
- WASI SDK installed as build toolchain (see below — provides clang/lld, not WASI runtime)
- Basic C programming knowledge

## Install WASI SDK (build toolchain)

```bash
WASI_VERSION=24
wget https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_VERSION}/wasi-sdk-${WASI_VERSION}.0-x86_64-linux.tar.gz
sudo tar xvf wasi-sdk-${WASI_VERSION}.0-x86_64-linux.tar.gz -C /opt
sudo ln -sf /opt/wasi-sdk-${WASI_VERSION}.0 /opt/wasi-sdk
```

> **Note:** The WASI SDK is used as a compiler toolchain only (clang + lld targeting `wasm32-unknown-unknown`). AkiraOS apps do **not** use WASI interfaces at runtime — install to `/opt/wasi-sdk` (the default path all sample Makefiles expect) or set `WASI_SDK=/your/path`.

---

## Hello World App

The `AkiraSDK/` submodule contains ready-to-build sample apps in `AkiraSDK/wasm_apps/`. The fastest way to get started is to copy an existing app as a template.

### Step 1: Create your project

```bash
cp -r ~/akira-workspace/AkiraOS/AkiraSDK/wasm_apps/hello_world ~/my_hello_world
cd ~/my_hello_world
```

### Step 2: Write the code

Apps include a single header — all APIs are declared there:

**main.c:**
```c
#include "akira_api.h"

int main(void) {
    printf("Hello from AkiraOS WASM!");

    display_clear(COLOR_BLACK);
    display_text(10, 10, "Hello AkiraOS!", COLOR_WHITE);
    display_flush();

    while (1) {
        delay(1000000);  // idle — yield every 1 second
    }
    return 0;
}
```

> **Key rules:**
> - Use `printf()` from `akira_api.h` — do **not** include `<stdio.h>`
> - All API functions are `extern` — they are provided by AkiraOS at runtime, not linked from a library
> - Always call `display_flush()` after drawing, or the screen stays blank
> - Always `delay()` in your loop — spinning without yielding burns 100% CPU

### Step 3: Create the manifest

**manifest.json:**
```json
{
  "name": "hello_world",
  "version": "1.0.0",
  "capabilities": ["display.write"],
  "memory_quota": 65536
}
```

The manifest is automatically embedded into the `.wasm` binary by `embed_manifest.py` (called by the Makefile).

### Step 4: Build

```bash
# Using the SDK build script (recommended)
cd ~/akira-workspace/AkiraOS/AkiraSDK/wasm_apps
./build.sh hello_world        # single app → bin/hello_world.wasm
./build.sh                    # all apps

# Or with Make directly from your app directory
cd ~/my_hello_world
make
```

Verify the output:
```bash
ls -lh bin/hello_world.wasm   # should be ~2–5 KB
file bin/hello_world.wasm     # "WebAssembly (wasm) binary module"
```

---

## Deploy to Hardware

Three methods are available; SD card is the simplest for development.

### Option 1: SD Card (simplest)

Prepare a FAT32-formatted microSD card (see [SD Card Setup](../hardware/sd-card.md)), copy your `.wasm` into the `apps/` directory, insert the card, and scan:

```bash
# On your PC — copy the app to the SD card
cp bin/hello_world.wasm /media/$USER/AKIRA/apps/

# On the device shell
AkiraOS:~$ app scan sd
AkiraOS:~$ app install hello_world
AkiraOS:~$ app run hello_world
```

### Option 2: HTTP Upload (over WiFi)

The device must be connected to WiFi. Find its IP from the serial console (`net iface`).

```bash
# The app binary is streamed raw to the app-manager install route.
# (/upload is the firmware-flash endpoint and must not be used for apps.)
curl -X POST --data-binary @bin/hello_world.wasm \
  "http://<device-ip>/api/apps/install?name=hello_world"
```


Expected output:
```
[00:00:10.523] <inf> wasm: Loading: /apps/hello_world.wasm
[00:00:10.687] <inf> wasm: App loaded: hello_world
[00:00:10.692] <inf> app: Hello from AkiraOS WASM!
```

---

## Next Steps

### Read a sensor

```c
#include "akira_api.h"

int main(void) {
    int raw = sensor_read(SENSOR_CHAN_AMBIENT_TEMP);
    if (raw == AKIRA_SENSOR_ERROR) {
        printf("sensor unavailable");
    } else {
        // raw = 23500 → 23.5 °C; divide by 1000 for whole degrees
        printf("Temp: %d.%d C", raw / 1000, (raw % 1000) / 100);
    }
    return 0;
}
```

Capability required: `"sensor.read"`

### Read a GPIO input

```c
#include "akira_api.h"

#define BTN_PIN 0   // adjust to your hardware

int main(void) {
    gpio_configure(BTN_PIN, GPIO_INPUT | GPIO_PULL_DOWN);

    while (1) {
        if (gpio_read(BTN_PIN) == 1) {
            printf("button pressed!");
        }
        delay(10000);  // poll every 10 ms
    }
    return 0;
}
```

Capability required: `"gpio.read"`

### Write to persistent storage

```c
#include "akira_api.h"

int main(void) {
    int fd = storage_open("message.txt", STORAGE_O_WRITE);
    if (fd >= 0) {
        storage_write(fd, "Hello, filesystem!", 18);
        storage_close(fd);
        printf("saved");
    }
    return 0;
}
```

Capability required: `"storage.write"`

---

## Troubleshooting

**"Failed to load WASM"** — check the file exists:
```bash
AkiraOS:~$ fs ls /apps
```

**"capability denied"** — add the required capability to `manifest.json`:
```json
{ "capabilities": ["display.write", "sensor.read"] }
```

**"Out of memory"** — increase the quota in `manifest.json`:
```json
{ "memory_quota": 131072 }
```

**Screen stays black** — you forgot `display_flush()` after your draw calls.

**Function not found at build time** — do not include `<stdio.h>` or other stdlib headers. All functions come from `akira_api.h`.

---

## Related Documentation

- [SDK API Reference](../../AkiraSDK/docs/API_REFERENCE.md) — every function with examples
- [Best Practices](../../AkiraSDK/docs/BEST_PRACTICES.md) — main loop patterns, memory, display
- [Manifest Format](../api-reference/manifest-format.md) — capability specification
- [Building Apps](../development/building-apps.md) — AOT compilation and advanced topics
