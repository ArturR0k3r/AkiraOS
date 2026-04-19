<div align="center">

<img src="assets/logo.png" alt="AkiraOS Logo" width="300"/>

# AkiraOS

**WebAssembly Runtime for Microcontrollers**

Every app is a sandboxed `.wasm` module. Deploy over-the-air. No firmware flash required.

[![Version](https://img.shields.io/badge/version-1.4.9%20"Gl1tch"-7f5af0?style=flat-square)](https://github.com/ArturR0k3r/AkiraOS/releases)
[![Zephyr](https://img.shields.io/badge/Zephyr-4.3.0-3b82f6?style=flat-square)](https://zephyrproject.org)
[![WAMR](https://img.shields.io/badge/WAMR-2.x-22c55e?style=flat-square)](https://github.com/bytecodealliance/wasm-micro-runtime)
[![License](https://img.shields.io/badge/license-Apache%202.0-22c55e?style=flat-square)](LICENSE)
[![OSHWA](https://img.shields.io/badge/OSHWA-MD000003-f59e0b?style=flat-square)](https://certification.oshwa.org/md000003.html)
[![Stars](https://img.shields.io/github/stars/ArturR0k3r/AkiraOS?style=flat-square&color=7f5af0)](https://github.com/ArturR0k3r/AkiraOS/stargazers)

[**Quick Start**](#quick-start) · [**Architecture**](#architecture) · [**AkiraSDK**](#wasm-app-development) · [**Hardware**](#supported-hardware) · [**Docs**](https://docs.akiraos.dev)

</div>

---

## What is AkiraOS?

AkiraOS is a Zephyr-based embedded OS that runs **sandboxed WebAssembly applications** on microcontrollers.

The core idea: separate the OS from the application. The firmware stays stable. Apps are `.wasm` binaries — isolated, portable, and deployable over-the-air without touching the OS.

```
Your App (C/C++/Python)  →  compile  →  app.wasm  →  SecureDeploy  →  runs on device
                                                              OS unchanged
```

**Why this matters:**
- Update apps in the field without a firmware flash cycle
- One binary runs on ESP32-S3, nRF5x, or STM32 — no recompile
- Bad app crashes? Runtime catches it at the boundary. Device stays up.
- Every app gets only the hardware access it explicitly requested

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│              USER SPACE — WASM Apps                 │
│   [app1.wasm]   [app2.wasm]   [your_app.wasm]       │
│   50KB–200KB per app · max 8 installed · 2 running  │
└──────────────────────┬──────────────────────────────┘
                       │ capability-checked calls
┌──────────────────────▼──────────────────────────────┐
│              AKIRAZ RUNTIME                         │
│  App Manager · Capability Guard · Native Bridge     │
│  UI Framework (32 widgets) · Shell · 18 API modules │
│  WAMR: interpreter (1x) or AOT (10–50x perf)        │
└──────────┬──────────────────────┬───────────────────┘
           │                      │
┌──────────▼──────────┐  ┌───────▼───────────────────┐
│  CONNECTIVITY       │  │  ZEPHYR RTOS               │
│  HTTP · OTA         │  │  Scheduler · Network Stack  │
│  BLE · AkiraMesh    │  │  Drivers · LittleFS         │
└─────────────────────┘  └────────────────────────────┘
```

**Capability Guard** — every native API call goes through an inline permission check (~60ns overhead). Apps declare required capabilities in a manifest. No capability = no access. Period.

```json
{
  "name": "my_sensor_app",
  "capabilities": ["gpio.read", "display.write", "sensor.read"]
}
```

Full architecture docs → [docs.akiraos.dev/architecture](https://docs.akiraos.dev/architecture)

---

## Quick Start

> **No hardware? No problem.** AkiraOS runs on `native_sim` — test everything on your Linux host first.

### Prerequisites

- Linux or WSL2 (Ubuntu 20.04+)
- Python 3.8+
- `west` — `pip install west`

### 1 — Clone

```bash
mkdir akira-workspace && cd akira-workspace
git clone --recurse-submodules https://github.com/ArturR0k3r/AkiraOS.git
cd AkiraOS
west init -l . && cd .. && west update
```

### 2 — Install Zephyr SDK

```bash
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.4/zephyr-sdk-0.17.4_linux-x86_64.tar.xz
tar xvf zephyr-sdk-0.17.4_linux-x86_64.tar.xz
cd zephyr-sdk-0.17.4 && ./setup.sh
```

### 3 — Build and run

```bash
cd akira-workspace/AkiraOS

# Run on your host — no hardware needed
./build.sh

# Build and flash to ESP32-S3
cd .. && west blobs fetch hal_espressif && cd AkiraOS
./build.sh -b esp32s3_devkitm_esp32s3_procpu -r a
west espmonitor
```

Full setup guide → [QUICKSTART.md](QUICKSTART.md)

---

## WASM App Development

Install the [WASI SDK](https://github.com/WebAssembly/wasi-sdk/releases), then:

```bash
# Build your first app
cd AkiraSDK/wasm_apps/hello_world
../../build_wasm_app.sh -o hello_world.wasm main.c

# Deploy to a running device over WiFi — no reflash needed
curl -X POST -F "file=@hello_world.wasm" http://<device-ip>/upload
```

**Example apps in `AkiraSDK/wasm_apps/`:**

| App | Description |
|-----|-------------|
| `hello_world` | Minimal starter |
| `sensor_demo` | Read hardware sensors |
| `display_graphics` | Graphics rendering |
| `gui_demo` | Full UI with 32 widget types |
| `hid_keyboard_demo` | USB HID input |
| `blink_led` | GPIO control |
| `logic_analyzer` | Data capture |

**Native API Modules (18 total):** BLE · Display · GPIO · HID · I2C · IPC · Lifecycle · Memory · Net · Power · PWM · RF · Sensor · Storage · Timer · UART · and more.

Full API reference → [docs.akiraos.dev/api-reference](https://docs.akiraos.dev/api-reference)

---

## Supported Hardware

| Platform | Status | Architecture | Tier | Notes |
|----------|--------|-------------|------|-------|
| **AkiraConsole** | ✅ Supported | Xtensa LX7 | Tier 1 | ESP32-S3 · Custom HW |
| ESP32 | ✅ Supported | Xtensa LX7 / RISC-V | Tier 1 | -S3 (LX7) · -H2 · -C6 (RISC-V) |
| native\_sim | ✅ Supported | Host (x86\_64) | Tier 1 | Fast iteration, no hardware needed |
| nRF54L15 | ✅ Supported | ARM Cortex-M33 | Tier 2 | BLE 5.4 · Nordic |
| STM32 | ✅ Supported | ARM Cortex-M | Tier 2 | B-U585I-IOT02A · STEVAL-STWINBX1 · H753 · H723 |


**Recommended:** ESP32-S3 DevKitM — or [AkiraConsole V3](https://akiraos.dev/akiraconsole) (coming to CrowdSupply).

---

## AkiraConsole V3

The reference hardware platform for AkiraOS.

**OSHWA Certified — UID: [MD000003](https://certification.oshwa.org/md000003.html)**
Rev A.2 · Engineering samples in production · CrowdSupply campaign coming soon.

→ ESP32-S3 dual-core 240MHz · 8MB PSRAM
→ TFT display · ~33 FPS in AkiraOS
→ 8 tactile buttons · Rotary encoder
→ CC1121 sub-GHz radio · LoRa
→ MicroSD · USB-C · Expansion headers

[akiraos.dev/akiraconsole](https://akiraos.dev/akiraconsole)

---

## What's in v1.4.9 "Gl1tch"

125 commits · 350 files · ~40,600 lines of changes

- **WAMR Runtime** replaces legacy OCRE engine
- **Capability Guard** security model — per-app permission enforcement  
- **Full WASM Peripheral API** — GPIO, Display, BLE, HID, Sensors, Storage
- **AkiraSDK** as independent git submodule
- **AkiraConsole** board bringup complete

[Full changelog →](CHANGELOG.md)

---

## Build System

```bash
./build.sh -b esp32s3_devkitm_esp32s3_procpu        # ESP32-S3
./build.sh -b esp32_devkitc_procpu                  # ESP32
./build.sh -b esp32c3_devkitm                       # ESP32-C3
./build.sh -b native_sim                            # Simulation
./build.sh -b esp32s3_devkitm_esp32s3_procpu -r all # Clean rebuild
```

Key config files: `prj.conf` · `boards/*.conf` · `boards/*.overlay` · `west.yml`

---

## Security Model

1. **WASM Sandboxing** — no direct memory access to kernel space, stack/heap isolated per app
2. **Capability Guard** — inline checks on every native API call, manifest-declared permissions
3. **Secure Boot** — MCUboot validates firmware signature, WAMR validates module checksum
4. **OTA Security** — SHA-256 integrity, atomic updates, rollback on failure

[Security architecture →](https://docs.akiraos.dev/architecture/security.html)

---

## Contributing

```bash
git checkout -b feature/your-feature
./build.sh                                           # test on native_sim first
./build.sh -b esp32s3_devkitm_esp32s3_procpu -r a   # then on hardware
```

See [CONTRIBUTING.md](CONTRIBUTING.md) · Code style: Zephyr C · Commits: conventional format

---

## Links

| | |
|--|--|
| 📖 Docs | [docs.akiraos.dev](https://docs.akiraos.dev) |
| 🖥️ Hardware | [akiraos.dev/akiraconsole](https://akiraos.dev/akiraconsole) |
| 🏷️ OSHWA | [certification.oshwa.org/md000003.html](https://certification.oshwa.org/md000003.html) |
| 💬 Discussions | [GitHub Discussions](https://github.com/ArturR0k3r/AkiraOS/discussions) |
| 📢 Telegram | [@theguywithpen](https://t.me/theguywithpen) |
| 🛒 CrowdSupply | Coming soon — [akiraos.dev/akiraconsole](https://akiraos.dev/akiraconsole) |

---

## Acknowledgments

[Zephyr Project](https://zephyrproject.org) · [Bytecode Alliance / WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) · [Espressif Systems](https://espressif.com) · [Nordic Semiconductor](https://nordicsemi.com) · [MCUboot](https://mcuboot.com)

---

<div align="center">

**If AkiraOS is useful to you — a ⭐ helps others find it.**

[Star on GitHub](https://github.com/ArturR0k3r/AkiraOS) · [Follow updates](https://t.me/theguywithpen) · [Docs](https://docs.akiraos.dev)

*Apache 2.0 · Copyright © 2026 PenEngineering S.R.L · [Commercial licenses available](COMMERCIAL.md)*

</div>
