<div align="center">

<img src="assets/logo.png" alt="AkiraOS Logo" width="300"/>

# AkiraOS

**Write firmware apps, not firmware.**
Sandboxed WebAssembly apps for microcontrollers, deployed over the air.

[![Version](https://img.shields.io/badge/version-1.6.7-7f5af0?style=flat-square)](https://github.com/ArturR0k3r/AkiraOS/releases)
[![Zephyr](https://img.shields.io/badge/Zephyr-4.3.0-3b82f6?style=flat-square)](https://zephyrproject.org)
[![WAMR](https://img.shields.io/badge/WAMR-2.x-22c55e?style=flat-square)](https://github.com/bytecodealliance/wasm-micro-runtime)
[![License](https://img.shields.io/badge/license-Apache%202.0-22c55e?style=flat-square)](LICENSE)
[![OSHWA](https://img.shields.io/badge/OSHWA-MD000003-f59e0b?style=flat-square)](https://certification.oshwa.org/md000003.html)

[**Docs**](https://docs.akiraos.dev) · [**Quick Start**](QUICKSTART.md) · [**Architecture**](https://docs.akiraos.dev/architecture) · [**Contributing**](CONTRIBUTING.md) · [**Roadmap**](ROADMAP.md)

<br/>

### [▶ Try in your browser](https://console.app.akiraos.dev/simulator) &nbsp;·&nbsp; [🕹 Browse apps](https://console.app.akiraos.dev) &nbsp;·&nbsp; [⚡ Flash your board](#60-second-start)

<!-- TODO(assets): record a short screen capture of the browser simulator and save it
     as assets/simulator.gif, then uncomment the line below. -->
<!-- ![AkiraOS browser simulator](assets/simulator.gif) -->

</div>

---

## What is AkiraOS?

AkiraOS is a Zephyr-based embedded OS that runs **sandboxed WebAssembly applications** on
microcontrollers. It separates the OS from the application: the firmware stays stable, and
apps are `.wasm` binaries — isolated, portable, and installable over the air without
reflashing the OS.

```
Your app (C / Rust)  →  compile  →  app.wasm  →  install over WiFi/BLE/USB  →  runs sandboxed
                                                                      OS unchanged
```

- **Update apps in the field** without a firmware flash cycle.
- **One binary runs** on ESP32-S3, nRF54L15 or RP2350 — no per-board recompile.
- **A crashing app can't take the device down** — the runtime catches it at the boundary.
- **Every app gets only the hardware it asked for**, enforced by a capability manifest.

---

## 60-second start

**No install — run in your browser:** open the
[AkiraConsole simulator](https://console.app.akiraos.dev/simulator), pick an app from the
[catalogue](https://console.app.akiraos.dev) or drop in your own `.wasm`.

**Build an app and run it on `native_sim` (no hardware):**

```bash
git clone --recurse-submodules https://github.com/ArturR0k3r/AkiraOS.git
cd AkiraOS && west init -l . && cd .. && west update      # one-time workspace setup
cd AkiraOS && ./build.sh                                   # build + run native_sim

cd AkiraSDK/wasm_apps && ./build.sh hello_world            # -> bin/hello_world.wasm
```

**Install an app on a device over WiFi** (no reflash):

```bash
curl -X POST --data-binary @bin/hello_world.wasm \
  "http://<device-ip>/api/apps/install?name=hello_world"
```

Full guide → [QUICKSTART.md](QUICKSTART.md).

---

## Your first app

**C** — a minimal app ([more samples](https://github.com/ArturR0k3r/AkiraSDK/tree/v1.6.x/wasm_apps)):

```c
#include "akira_api.h"

int main(void) {
    display_clear(COLOR_BLACK);
    display_text(10, 10, "Hello from C", COLOR_CYAN);
    display_flush();
    return 0;
}
```

**Rust** ([`AkiraSDK/wasm_apps/rust/hello_world`](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/wasm_apps/rust/hello_world/src/main.rs)):

```rust
#![no_std]
#![no_main]
use akira_sdk::display;

#[no_mangle]
pub extern "C" fn main() -> i32 {
    display::clear(display::COLOR_BLACK);
    display::text(10, 10, "Hello from Rust", display::COLOR_CYAN);
    display::flush();
    0
}
```

Each app ships a `manifest.json` declaring the capabilities it needs (here, `display.write`).
C is fully supported; **Rust and MicroPython are experimental**
([language support](https://docs.akiraos.dev/development/building-apps.html#language-support)).

---

## Why AkiraOS

1. **Ship app updates, not firmware.** Push a `.wasm` over WiFi/BLE/USB; the OS and bootloader stay put. No full reflash, no bricking on a bad app build.
2. **Capability-based isolation.** Each app runs in a WASM sandbox and may call only the native APIs its manifest declares — enforced on every call.
3. **One app binary, many MCUs.** The same `.wasm` runs on Xtensa and ARM; ship an optional `.aot` per architecture for native-code speed.
4. **A real app ecosystem.** Browser simulator, app catalogue, an SDK (C / Rust / MicroPython) and reference hardware — not just a runtime.

---

## How it compares

| | **AkiraOS** | MicroPython | Toit | Ocre | Arduino / ESP-IDF |
|---|---|---|---|---|---|
| App unit | `.wasm` module | `.py` / `.mpy` script | Toit container | `.wasm` container | full firmware image |
| Update without reflashing the OS | ✅ | ✅ | ✅ | ✅ | ❌ (reflash) |
| Per-app sandbox | ✅ WASM (+ optional MPU) | ❌ shared VM | ✅ | ✅ WASM | ❌ |
| Capability permissions | ✅ manifest | ❌ | partial | limited | ❌ |
| Same binary across MCU archs | ✅ | ✅ bytecode | partial | ✅ | ❌ recompile |
| App languages | C, Rust, (MicroPython) | Python | Toit | any → WASM | C / C++ |
| License | Apache-2.0 | MIT | open core | Apache-2.0 | mixed |

[Ocre](https://lfedge.org/projects/ocre/) is the closest — also WAMR-on-Zephyr. AkiraOS adds
the capability manifest model, reference hardware (AkiraConsole), an app catalogue and the
browser simulator.

---

## Supported hardware

| Platform | Status | Arch | Notes |
|----------|--------|------|-------|
| **AkiraConsole** | ✅ | Xtensa LX7 | ESP32-S3 reference device · [OSHWA MD000003](https://certification.oshwa.org/md000003.html) |
| ESP32-S3 / -C3 / -C6 / -H2 | ✅ | LX7 / RISC-V | DevKits; C6/H2 build in CI, `build.sh` can't flash them yet |
| nRF54L15 | ✅ | Cortex-M33 | BLE 5.4 |
| STM32 (U5 / H7 / L4) | ✅ | Cortex-M | B-U585I-IOT02A · STEVAL-STWINBX1 · Nucleo-L476RG |
| RP2040 / RP2350 | 🧪 | Cortex-M0+/M33 | Pico / Pico 2 — flash the UF2 manually |
| native_sim | ✅ | host x86-64 | fast iteration, no hardware |

Prebuilt firmware is attached to each
[release](https://github.com/ArturR0k3r/AkiraOS/releases). **Recommended:** ESP32-S3 DevKitM
or [AkiraConsole](https://akiraos.dev/akiraconsole) (coming to CrowdSupply).

---

## Architecture

```
┌──────────────────────────────────────────────────────┐
│   USER SPACE — WASM apps   [app1.wasm] [your_app.wasm] │
│   Kconfig defaults: 8 installed · 2 running            │
└───────────────────────────┬──────────────────────────┘
                            │ capability-checked native calls
┌───────────────────────────▼──────────────────────────┐
│   AkiraOS runtime                                      │
│   App Manager · Capability Guard · Native API bridge   │
│   WAMR: classic interpreter (.wasm) or AOT (.aot)      │
├───────────────────────────┬──────────────────────────┤
│   Connectivity (WiFi/BLE/  │   Zephyr RTOS             │
│   OTA/mesh)                │   scheduler · drivers · FS │
└────────────────────────────┴─────────────────────────┘
```

Every native API call checks the calling app's capability mask before it runs. Apps declare
what they need in a manifest; anything ungranted is denied.

Full architecture, app limits and the WASM/AOT model →
[docs.akiraos.dev/architecture](https://docs.akiraos.dev/architecture). No performance
figures are published yet — see [Performance](https://docs.akiraos.dev/resources/performance.html).

---

## Security

1. **WASM sandboxing** — apps have no direct access to kernel memory; heap and stack are
   per-app.
2. **Capability Guard** — every native call is checked against the app's manifest-declared
   capabilities.
3. **MCUboot** — dual-slot A/B updates with SHA-256 image integrity and boot-failure
   rollback (`boot_guard`).
4. **Signed releases** — `./build.sh --release -k <key>` (and the release CI with the
   `MCUBOOT_KEY_PEM` secret) sign the MCUboot image and enable downgrade prevention. The
   SESIP Level 2 evaluated configuration (tag `v1.6.6`) ships this way.

> **Honest hardening status.** Default developer builds are **unsigned**; authenticity comes
> from the `--release` signing flow above. In-tree **app-signature** verification is a hook
> (`CONFIG_AKIRA_REQUIRE_SIGNED_APPS` + `CONFIG_AKIRA_HARDEN_UNATTESTED_APPS`); a full
> verifier ships with AkiraPlatform. ESP32 **flash encryption** and **Secure Boot V2** are
> integrator steps, not enabled by default. Treat these as configuration you must turn on for
> a fielded device.

[Security architecture →](https://docs.akiraos.dev/architecture/security.html)

---

## Open source vs commercial

AkiraOS firmware is **Apache-2.0 and free forever** — the full runtime, every WASM API, the
AkiraSDK and samples, all hardware targets, with no fees. Your app code stays proprietary
(Apache-2.0 is not copyleft).

Paid tiers add hosted services, not firmware features: **AkiraHub** (device registry, app
catalogue, remote install), the `akira-cli` signing service, enterprise support and legal
indemnification. See [COMMERCIAL.md](COMMERCIAL.md).

---

## Contributing

```bash
git checkout -b feature/your-change
./build.sh                                           # test on native_sim first
./build.sh -b esp32s3_devkitm/esp32s3/procpu -r a    # then on hardware
```

Good first issues and how to **publish an app to the AkiraConsole App Store** are in
[CONTRIBUTING.md](CONTRIBUTING.md). Day-to-day work is on `v1.6.x`; PRs target `main`.

---

## Links

| | |
|--|--|
| 📖 Docs | [docs.akiraos.dev](https://docs.akiraos.dev) |
| 🕹 Apps & simulator | [console.app.akiraos.dev](https://console.app.akiraos.dev) |
| 🖥️ Hardware | [akiraos.dev/akiraconsole](https://akiraos.dev/akiraconsole) |
| 💬 Discussions | [GitHub Discussions](https://github.com/ArturR0k3r/AkiraOS/discussions) |
| 💬 Chat | Discord / Matrix — _coming soon (TODO: invite links)_ |
| 📢 Telegram | [@theguywithpen](https://t.me/theguywithpen) |

---

<div align="center">

**If AkiraOS is useful to you, a ⭐ helps others find it.** · Apache 2.0 · © 2026 PenEngineering S.R.L · [Commercial licenses](COMMERCIAL.md)

</div>
