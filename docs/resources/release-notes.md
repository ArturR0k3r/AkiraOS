---
layout: default
title: Release Notes
parent: Resources
nav_order: 5
permalink: /resources/release-notes
---

# Release Notes

## v1.4.9 — "Gl1tch"

**Released:** March 2026
**Branch:** `v1.4.x` → `main` | **PR:** [#52](https://github.com/ArturR0k3r/AkiraOS/pull/52)

This release completes the v1.4.x generation of AkiraOS. It replaces the legacy OCRE runtime with WAMR, introduces the Capability Guard security model, ships the AkiraConsole custom board, adds a comprehensive suite of WASM peripheral APIs, and delivers the AkiraSDK as an independently versioned git submodule alongside a full documentation site.

---

### Runtime Engine

The core execution engine has been completely rewritten around **WebAssembly Micro Runtime (WAMR)**.

**What changed:**
- Full OCRE → WAMR migration with Zephyr 4.3.0 compatibility
- Thread-per-app model — each WASM app runs in its own Zephyr thread managed by `SYS_INIT`
- AOT (Ahead-of-Time) compilation enabled by default for native-speed execution and ~60 KB RAM savings per app slot compared to the interpreter
- Chunked WASM loading with PSRAM fallback on ESP32-S3
- Instance pool and SHA-256-keyed module cache eliminate redundant reloads
- Per-app execution watchdog with configurable timeout
- WASM apps now compile to bare `wasm32` — no WASI dependency

**Bug fixes:**
- WASM out-of-bounds memory trap on ESP32-S3
- App lifecycle freezes on repeated start/stop cycles
- Boot crash when SD card is absent but storage is enabled
- DRAM overflow on non-PSRAM boards

---

### Security Model

AkiraOS v1.4.9 introduces a formal **Capability Guard** security architecture.

Every native API export is guarded by `akira_security_check_exec()`. Apps declare required capabilities (`AKIRA_CAP_*` bitmasks) in their manifest JSON; the runtime enforces these at dispatch time.

Additional hardening:
- **SHA-256 binary integrity** — WASM binary hash verified before loading (mbedTLS)
- **Ed25519/RSA signature verification** — publisher signatures checked via mbedTLS (`app_signing_v2.c`)
- **Runtime sandbox** — syscall filtering and per-app call rate limiting (`sandbox.c`)
- **Security audit log** — ring-buffer log of capability violations for post-mortem analysis

For capability definitions see [Manifest Format](../api-reference/manifest-format.md) and [Native API Reference](../api-reference/native-api.md).

---

### Connectivity

#### BLE
The low-level `bt_shell` interface is replaced by a full **Arduino-style BLE App API** callable from WASM. Apps can create GATT services, register characteristics, advertise, read/write values, and receive connection events through a simple event queue.

Key functions: `ble_init`, `ble_service_create`, `ble_char_create`, `ble_advertise`, `ble_char_write`, `ble_event_pop`.

#### Bluetooth HID
Full BT HID WASM API for keyboard/media hotkey scripting from WASM apps. iOS compatibility fixed for BLE HID.

#### USB HID
Ported to the new Zephyr USB stack. Mouse device added alongside keyboard.

#### Network (TCP/UDP)
New zero-copy network API using shared-memory ring buffers. Functions: `net_tcp_connect`, `net_tcp_send`, `net_udp_send`, `net_recv`.

#### HTTP Web Server
- Crash on first request fixed
- Shared buffer pool added for upload operations
- Callback-based transport layer refactor

#### OTA
Partition layout corrected — NVS/LFS now reside above the OTA region, preventing partition overlap. Stale boot counter healed automatically on upgrade.

---

### Hardware & Board Support

#### New Boards

| Board | SoC | Notes |
|---|---|---|
| `akiraconsole_esp32s3_procpu` | ESP32-S3 | Custom AkiraConsole hardware — full HWMv2 out-of-tree definition |
| `esp32s3_super_mini_esp32s3_procpu` | ESP32-S3 | Compact form-factor; HWMv2-compatible |

#### New Drivers

| Driver | Binding | Description |
|---|---|---|
| ILI9341 | `akira,ili9341` | Out-of-tree display driver for AkiraConsole |
| LR1121 | `akira,lr1121` | Sub-GHz/2.4 GHz RF transceiver |
| LSM6DS3 | Out-of-tree | 6-axis IMU with generic sensor WASM API facade |

#### Other Hardware Changes
- SD card support added; apps can be installed directly from SD
- ST7789V integrated with Zephyr native driver and capability registration
- STM32 B-U585I: boot sequence fixed; app image signed for MCUBoot
- All board `.conf` / `.overlay` files refactored for strict modularity

---

### WASM API Surface

v1.4.9 adds 13 new API domains exposed to sandboxed WASM apps:

| Domain | Representative Functions |
|---|---|
| GPIO / Input | `gpio_set`, `gpio_get`, `button_event_pop` |
| Timer | `timer_create`, `timer_start`, `timer_stop` |
| UART / I2C / PWM | `uart_write`, `i2c_read`, `i2c_write`, `pwm_set` |
| Display | `display_fill`, `display_draw_rect`, `display_blit` |
| Sensor | `sensor_read`, `sensor_start` |
| BLE | 12-function BLE App API |
| BT HID | `bt_hid_send_key`, `bt_hid_send_media` |
| Network | `net_tcp_connect`, `net_tcp_send`, `net_udp_send`, `net_recv` |
| Storage / FS | `fs_open`, `fs_read`, `fs_write`, `fs_close`, `fs_list` |
| Power | `power_sleep`, `power_get_battery_level`, `power_get_charge_state` |
| RF | `rf_send`, `rf_recv`, `rf_set_channel` |
| App Lifecycle | `app_start`, `app_stop`, `app_get_state` |
| IPC | `ipc_send`, `ipc_recv` |

Full signatures and capability requirements are in the [Native API Reference](../api-reference/native-api.md).

---

### AkiraSDK

`wasm_sample/` has been replaced by the **AkiraSDK git submodule** (`AkiraSDK/`). The SDK is now versioned and distributed independently.

**Bundled WASM apps (17):**

| App | Description |
|---|---|
| `hello_world` | Minimal "Hello, World" WASM app |
| `gpio` | GPIO toggle and read demo |
| `ble_led` | BLE-controlled LED via GATT characteristic |
| `compass` | IMU-based compass visualization |
| `cube3d` | 3D rotating cube (display + IMU) |
| `display_test` | Display fill, text, and pattern test |
| `imu_3d` | Real-time 3D IMU orientation viewer |
| `imu_timer_test` | Timer-driven IMU sampling benchmark |
| `inclinometer` | Tilt/incline display from IMU |
| `logic_analyzer` | 4-channel logic analyzer v2.0 |
| `macro_pad` | BT HID macro pad / hotkey launcher |
| `net_echo` | TCP echo client/server |
| `net_server` | Simple HTTP server from WASM |
| `retro` | Retro-style game demo |
| `storage_test` | LittleFS read/write benchmark |
| `supervisor` | App lifecycle controller |
| `tetris` | Full playable Tetris for AkiraConsole |

---

### Build System & CI/CD

- `Dockerfile.ci` — hermetic Docker build environment with Espressif toolchain
- New `ci.yml` pipeline — parallel matrix across all supported boards; replaces old `build.yml`
- Zephyr SDK bumped to **0.17.4**
- `build.sh` rewritten — board auto-discovery, color help output, cleaner error messages
- GitHub issue templates and PR template added

---

### Documentation

This release ships the first production version of the AkiraOS documentation site at [docs.akiraos.dev](https://docs.akiraos.dev).

The site is built with Jekyll + Just the Docs (dark theme) and covers:
- Getting Started — hardware setup, first build, first WASM app
- Architecture — runtime internals, AOT compilation, security model, connectivity
- API Reference — native API, manifest format, error codes
- Development — build options, SDK usage, contributing
- Hardware — supported boards, schematics
- Resources — FAQ, glossary, performance benchmarks

---

### Resolved Issues

| Issue | Title |
|---|---|
| [#55](https://github.com/ArturR0k3r/AkiraOS/issues/55) | Integrate WAMR Core with AOT support |
| [#54](https://github.com/ArturR0k3r/AkiraOS/issues/54) | Implement AkiraRuntime Loader and App Manager |
| [#44](https://github.com/ArturR0k3r/AkiraOS/issues/44) | Security Enforcement Missing (Capabilities, Signing, Isolation) |
| [#19](https://github.com/ArturR0k3r/AkiraOS/issues/19) | Add Logic Analyzer app |
| [#13](https://github.com/ArturR0k3r/AkiraOS/issues/13) | Implement Meshtastic support on Akira |

---

### Contributors

- **[@ArturR0k3r](https://github.com/ArturR0k3r)** — architecture, WAMR runtime, security, board support, WASM APIs, AkiraSDK, CI/CD, docs
- **[@drxgoshh](https://github.com/drxgoshh)** — settings manager, filesystem, web server, connectivity, BT/USB HID

---

## v1.3.8 — "AkiraOS-v1.3.8-GL1TCH"

Legacy OCRE-based release. Superseded by v1.4.9.

---

## v1.2.3

Initial public release with OCRE runtime, basic BLE, WiFi OTA, and LittleFS storage.
