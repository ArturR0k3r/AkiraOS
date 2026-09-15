---
layout: default
title: ESP32-C3
parent: Platform Support
nav_order: 2
---

# ESP32-C3 Platform Guide

**Board Target:** `esp32c3_devkitm`

The ESP32-C3 is a single-core RISC-V microcontroller featuring WiFi and BLE 5.0. It is a lower-cost, highly power-efficient variant of the ESP32 family. Because it is severely RAM-constrained compared to the ESP32-S3 (400KB total SRAM), developing for this board on AkiraOS requires specific considerations.

## Hardware Specifications
- **CPU:** RISC-V 32-bit single-core @ 160MHz 
- **RAM:** 400KB SRAM (~32KB Heap available to AkiraOS via `CONFIG_HEAP_MEM_POOL_SIZE`)
- **Storage:** 4MB Flash
- **Connectivity:** 2.4 GHz WiFi (802.11 b/g/n) + Bluetooth 5 (LE)

## Configuration Profile Constraints (`boards/esp32c3_devkitm.conf`)

By default, AkiraOS implements severe runtime restrictions on this board to prevent memory starvation:

1.  **WiFi OR Bluetooth:** To fit within the 32KB heap pool, Bluetooth is explicitly disabled (`CONFIG_BT=n`). You must choose between `CONFIG_WIFI=y` OR `CONFIG_BT=y`; they cannot run simultaneously with the WASM Engine active.
2.  **Thread Stacks:** The Main thread stack is restricted to `3072 bytes`, and Workqueue stacks to `1536 bytes`. WASM App memory limits are tightly bound. 
3.  **App Manifest Quotas:** When writing WASM apps, you must strictly limit the app `memory_quota` in `.akira.manifest` to < `16000` bytes. 
4.  **Logging / Shell:** Shell history, tab completion, and wildcards are disabled to save RAM. Logging ring buffers are heavily truncated.

## Building and Flashing

Build the firmware using the standard West toolchain targeting the DevKitM config:

```bash
# From the workspace root:
west build -p -b esp32c3_devkitm AkiraOS -d build

# Flash via USB UART
west flash
```

---

*Last updated: 2026-09-14 (AkiraOS v1.6.5)*
