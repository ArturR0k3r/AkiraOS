---
layout: default
title: Release Notes
parent: Resources
nav_order: 5
permalink: /resources/release-notes
---

# Release Notes

> Release history. `CHANGELOG.md` at the repository root is the authoritative,
> commit-level record; this page summarises each release.

## v1.6.5 — Unreleased

**Status:** development series on the `v1.6.x` branch. `VERSION` tracks 1.6.5, but
**no 1.6.x git tag exists yet** — the newest tag is `v1.5.8`.

The 1.6 series is a connectivity build-out plus a production-readiness pass that
deliberately *removed* overclaims: entry points that were advertised but incomplete now
return `-ENOSYS`/`-ENOTSUP` instead of reporting fake success.

### Implemented (previously stubbed)
- **HTTP**: RFC 6455 WebSocket server (handshake, frame encode/decode, ping/pong/close) and
  static file serving with path-traversal protection (`CONFIG_AKIRA_HTTP_STATIC_FILES`).
- **CoAP**: DNS resolution, Content-Format parsing, and RFC 7959 Block2 GET / Block1 PUT.
- **OTA**: real HTTP(S) manifest fetch and firmware download with SHA-256 verification and
  anti-rollback.
- **Radio**: 802.15.4 raw receive via promiscuous mode with real per-frame LQI/RSSI.
- **Display**: LVGL input driven by real Zephyr input events (gpio-keys / D-pad / gamepad /
  touch) via `CONFIG_AKIRA_LVGL_INPUT_ZEPHYR`.
- **Sensors**: BME280 register writes over I2C (was `-ENOTSUP`).
- **Cloud**: real battery, CPU and memory telemetry replacing hardcoded values.

### Added
- **AkiraMesh**: AODV routing, end-to-end crypto, generic BLE transport, and a `mesh_*` WASM
  native API (`AKIRA_CAP_MESH`, bit 38).
- **MQTT / Home Assistant**: `mqtt_*` and `ha_light_*` natives with HA discovery
  (`AKIRA_CAP_MQTT`, bit 35). Requires `CONFIG_AKIRA_WASM_MQTT=y` or app imports fail to link.
- **BLE companion service** (`CONFIG_AKIRA_BT_COMPANION`) and an app command bridge.
- **BLE observer**: passive scan plus spam/spoof modes (`AKIRA_CAP_BLE_SCAN` bit 36,
  `AKIRA_CAP_BLE_SPAM` bit 37).
- **NXP SE050 secure element** driver (`CONFIG_AKIRA_SE050`, T=1'oI2C + SE05x APDU).
- **HID**: gamepad mode with an Xbox-model-1708-compatible wire format.
- **SD hotplug** via the `akira,sd-detect` devicetree node.
- **RF**: CC1121 OOK + raw capture/replay, LR2021 LoRa/FSK runtime selection.

### Security & hardening
- Fail-closed app verification (`CONFIG_AKIRA_REQUIRE_SIGNED_APPS`) and Dilithium-2 PQC
  verification path.
- Capability mask clamping for unsigned apps (`CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK`); fixed
  wildcard grants silently dropping bits above 35.
- Per-device AES-256 settings key derived from the hardware unique ID
  (`CONFIG_AKIRA_SETTINGS_PER_DEVICE_KEY`).
- OTA images verified by SHA-256 against authenticated metadata before being marked bootable.
- **Relicensed from GPL-3.0 to Apache-2.0**, with a CI SPDX/copyleft gate and a `NOTICE` file.

### Still stubbed
USB-host mass-storage mount, the Matter non-accessory path (needs the CHIP SDK), Thread
(needs OpenThread), and mesh WASM app distribution return `-ENOSYS`. **AkiraSync** has
Kconfig options and a reserved capability bit (39) but is not compiled into the firmware.

---

## v1.5.8

**Released:** 2026-05-29 · tag `v1.5.8"C1ph3r"`

- USB CDC serial interface and a BLE companion service for host pairing.
- BQ28Z610 fuel gauge driver with a `ti,bq28z610` DTS binding.
- WASM thread stacks pre-allocated via `SYS_INIT` to avoid runtime heap fragmentation.
- `nucleo_l476rg` board support.
- Watchdog hardening, HTTP upload auth, signing enforcement; legacy local web UI dropped.
- Fixed missing `NETWORK`/`SETTINGS` capability lookup entries, SD hot-plug reinit, and the
  deprecated `wdt_enable()` call (Zephyr 4.3 uses `wdt_setup()`).

See the [full v1.5.8 release notes](../release/v1.5.8.md).

---

## v1.5.6 — "C1PH3R"

**Released:** 2026-05-14

- WASM native APIs for Filesystem, Crypto and RTC, capability-gated in the sandbox.
- OTA delta update engine, boot guard with automatic rollback, and the OTA WASM native API.
- `akira` shell command tree, structured telemetry sink, and a panic handler with NVS crash storage.
- New capability bits 26–31: `FS_READ`/`FS_WRITE`, `CRYPTO`, `RTC_READ`/`RTC_WRITE`, `OTA_TRIGGER`.
- SD card hot-plug now works when the card was absent at boot.

---

## v1.5.4

**Released:** 2026-04-29

- API stability annotations (`@stability`, `@since`) on every public header, plus a formal
  2-release deprecation policy.
- Cooperative scheduling model and watchdog contract documented.
- Full ztest suite (54 tests) covering security, OTA, app lifecycle and the manifest parser,
  with GitHub Actions CI and Codecov coverage.

---

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

---

*Last updated: 2026-09-14 (AkiraOS v1.6.5)*
