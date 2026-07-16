# Changelog

All notable changes to AkiraOS are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.6.4] — Unreleased

> Development series on the `v1.6.x` branch. `VERSION` tracks 1.6.4; this section
> is backfilled from the commits since 1.5.8 plus the production-readiness
> hardening pass, and is not yet tagged/released.

### Implemented (previously stubbed)
- `feat(sensors)`: BME280 register writes over I2C (`i2c_reg_write_byte`) — was `-ENOTSUP`.
- `feat(cloud)`: Real device telemetry in the status payload — battery via the power
  manager, free heap via `sys_heap_runtime_stats_get`, CPU% via Zephyr thread runtime
  stats — replacing hardcoded `3700 mV / 85% / 0 / 0`.
- `feat(http)`: Static file serving from the filesystem (`CONFIG_AKIRA_HTTP_STATIC_FILES`,
  default n) with extension→Content-Type mapping and path-traversal protection.

### Security & hardening (production-readiness pass)
- `feat(ota)`: Verify a downloaded image's SHA-256 against authenticated metadata
  before it is ever marked bootable (`CONFIG_AKIRA_OTA_REQUIRE_HASH`,
  `ota_set_expected_sha256()`); the old 4-byte magic-only check is no longer
  sufficient.
- `feat(security)`: Clamp the capability mask an app can self-assert from its
  (unauthenticated) manifest — `"*"` is bounded to defined capabilities and
  unsigned apps are limited by `CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK`; privileged
  grants to unattested apps are audit-logged.
- `feat(settings)`: Derive a per-device AES-256 key from the hardware unique ID
  (`CONFIG_AKIRA_SETTINGS_PER_DEVICE_KEY`, fail-closed) instead of a shared
  compile-time key.
- `fix(license)`: Relicense first-party USB CDC serial + BLE companion service
  from GPL-3.0 to Apache-2.0; add a CI SPDX/copyleft gate and a `NOTICE` file.
- `docs`: Correct README secure-boot/OTA claims to the real (in-progress) status.
- `ci`: Version-consistency gate, expanded board matrix, pinned container image.
- Advertised-but-incomplete entry points now return `-ENOSYS`/`-ENOTSUP` instead
  of reporting fake success (pending full implementation, see below).

### Added
- `feat(security)`: Fail-closed app verification gate (`CONFIG_AKIRA_REQUIRE_SIGNED_APPS`).
- `feat(pqc)`: Dilithium-2 post-quantum signature verify path in the runtime.
- `feat(matter)`: Device-as-endpoint (accessory) support with an in-firmware mock co-processor for tests; accessory mock build enabled on esp32c6.
- `feat(mqtt)`: Home Assistant MQTT client integration (discovery + `ha_light_*`/`mqtt_*` WASM natives).
- `feat(mesh)`: AODV routing with reliable delivery and capability-based transport.
- `feat(radio)`: CC1121 OOK modulation, raw OOK capture/replay, continuous RX with frequency-offset registers; LR2021 LoRa modulation (runtime-selectable with FSK).
- `feat(api)`: HID modifier + FIDO/U2F Report ID 4 channel, AES-256-CTR, WiFi deauth, and raw RF capture/replay WASM APIs.
- `feat(board)`: esp32s3_super_mini — 2 MB Quad PSRAM + WiFi, LEDC PWM, and WASM BLE RGB controller.
- `feat(shell)`: `bt gatt` command to dump registered GATT services.
- `feat(bench)`: `akiraclaw_bench` hardware benchmark suite.

### Fixed / Security
- `fix(ble)`: Require an encrypted link for privileged management writes; make WASM app GATT services discoverable.
- `fix(security)`: Consistent `-EPERM` for capability denial; map `ota.trigger` capability string to its mask.
- `fix(wasm)`: Centralized, validated WASM pointer+length translation across HID/RF native APIs; decrement per-app memory quota on `mem_free`.
- `fix(ws)`: Connect `wss://` over TLS instead of silent cleartext.
- `fix(fs)`: Fail loud instead of diverting persistent writes to RAM.
- `fix(akiraconsole_prod)`: Fit `dram0.bss` and right-size the BT system heap to restore BLE.
- `fix(boards)`: Restore builds for rpi_pico(2), nucleo_h743zi, xiao_esp32c6.

### Changed
- `fix(version)`: Derive `AKIRA_VERSION_*` from the `VERSION` file.
- `ci`: Codecov no-regression coverage gate.
- `refactor(ui)`: Remove the unused stub UI framework.

---

## [1.5.8] — 2026-05-29

### Added
- `feat(connectivity SPI)`: USB CDC serial interface (`usb_cdc_serial`) and BLE companion service for host pairing.
- `feat(power)`: BQ28Z610 fuel gauge driver with DTS binding (`ti,bq28z610`).
- `feat(runtime)`: Pre-allocate WASM thread stacks via `SYS_INIT` to prevent heap fragmentation at runtime.
- `feat(boot animation)`: Boot animation configurable via `CONFIG_AKIRA_BOOT_ANIMATION`, fps and colors tunable via Kconfig.
- `feat(boards)`: nucleo_l476rg board support.
- `feat(security)`: Watchdog hardening, HTTP upload auth, signing enforcement; drop legacy local web UI.
- `feat(connectivity)`: USB web serial interface and BLE companion service.

### Fixed
- `fix(security)`: Missing `NETWORK`/`SETTINGS` entries in capability name lookup table.
- `fix(storage)`: SD hot-plug — deinit before reinit, retry on init failure, case-insensitive `.wasm`/`.aot` scan.
- `fix(wdt)`: Replace deprecated `wdt_enable()` with `wdt_setup()` (Zephyr 4.3 API).
- `fix(connectivity)`: Miscellaneous BT transfer, HTTP, OTA, and settings fixes.
- `fix(build)`: All-board build — resolve Kconfig orphans, web server link errors, and display guards.
- `fix(build)`: Boot animation source guard; drop `lvgl_input_driver` from OS shell build.
- `fix(ci)`: `dorny/test-reporter` git exit 128 in test job.
- `refactor(config)`: Use native Zephyr driver selectors for display panel (remove manual `#define` guards).
- `style(akpkg)`: Reformat `akpkg.c` to project K&R coding style.

### Changed
- Version bumped from 1.5.6 → 1.5.8 across all files.

---

## [1.5.6] — "C1PH3R" — 2026-05-14

### Added
- Track B: WASM native APIs for Filesystem (`akira_fs_api`), Crypto (`akira_crypto_api`), and RTC (`akira_rtc_api`) with capability-gated sandbox.
- Track A: OTA delta update engine (`akira_delta`), boot guard with automatic rollback (`akira_boot_guard`), and OTA WASM native API (`akira_ota_api`).
- Track D: `akira` shell command tree (`akira_runtime_cmds`), structured telemetry sink (`akira_telemetry`), and panic handler with NVS crash storage (`akira_panic`).
- New capability bits 26–31: `AKIRA_CAP_FS_READ/WRITE`, `AKIRA_CAP_CRYPTO`, `AKIRA_CAP_RTC_READ/WRITE`, `AKIRA_CAP_OTA_TRIGGER`.
- `docs/ota_design.md` — OTA architecture design document.

### Fixed
- SD card hot-plug: if the card was absent at boot, inserting it later now works. `sd_manager_mount()` re-probes via `akira_sd_card_init()` instead of returning `-ENODEV` immediately, and `fs_manager_reinit_sd()` updates the availability flag so all SD paths become accessible without reboot.
- `ota_request_rollback()` implemented (was declared but never defined).
- `ota_manager.c` compile guard aligned with `CONFIG_AKIRA_OTA` (fixes undeclared `CONFIG_AKIRA_OTA_THREAD_STACK_SIZE` on boards with OTA disabled).
- Pre-existing warnings: nested `/*` in Doxygen comments, `printf` format mismatch (`off_t` vs `%x`), unused static functions.
- All WASM allocations migrated to `akira_malloc_buffer()` / `akira_free_buffer()`.

### Changed
- Version bumped from 1.5.4 → 1.5.6 across all files.
- Log banner updated to `AkiraOS v1.5.6 `.

---

## [1.5.4] — 2026-04-29

### Added
- API stability annotations (`@stability`, `@since`) on every public header.
- `docs/api-stability-policy.md` — formal 2-release deprecation policy.
- Cooperative scheduling model and watchdog contract documented in
  `docs/architecture/scheduling-and-watchdog.md`.
- Full ztest suite (54 tests) covering security, OTA, app lifecycle,
  manifest parser; GitHub Actions CI with gcov/Codecov coverage badge.

### Changed
- Version bumped from 1.5.1 → 1.5.4 across all files.
- Log banner updated to `AkiraOS v1.5.4 - Hardened Runtime`.

---

## [1.4.9] — "Gl1tch" — 2026-03-21

> **Release branch:** `v1.4.x` | **PR:** [#52](https://github.com/ArturR0k3r/AkiraOS/pull/52)
> 125 commits · 350 files · ~40,600 insertions

This release completes the v1.4.x generation of AkiraOS. It replaces the legacy OCRE runtime with WAMR, introduces the Capability Guard security model, brings up the AkiraConsole custom board, adds a full suite of WASM peripheral APIs, and ships the AkiraSDK as an independently versioned git submodule.

**Closes:** [#55](https://github.com/ArturR0k3r/AkiraOS/issues/55) · [#54](https://github.com/ArturR0k3r/AkiraOS/issues/54) · [#44](https://github.com/ArturR0k3r/AkiraOS/issues/44) · [#19](https://github.com/ArturR0k3r/AkiraOS/issues/19) · [#13](https://github.com/ArturR0k3r/AkiraOS/issues/13)

---

### Runtime Engine

#### Added
- **WAMR integration** — complete migration from OCRE to WebAssembly Micro Runtime (WAMR) with Zephyr 4.3.0 compatibility and `native_sim` support
- **Thread-per-app model** — each WASM app runs in its own Zephyr thread; lifecycle managed via `SYS_INIT` hooks
- **AOT support** enabled by default — native-level execution speed with ~60 KB RAM savings over interpreter mode
- **Chunked WASM loading** with automatic PSRAM fallback on ESP32-S3; prevents boot-time OOM
- **Instance pool** — pre-allocated memory slots avoid repeated `malloc`/`free` cycles during app churn
- **Module cache** — SHA-256-keyed hash map skips redundant reloads of identical WASM binaries
- **Execution watchdog** — per-slot configurable timeout terminates runaway apps automatically
- **Per-app memory quotas** enforced inline at `NativeSymbol` dispatch layer
- **Inline capability checks** — `AKIRA_CAP_*` bitmask validated before every native export call
- `wamr_config.h` — central WAMR compile-time configuration header
- `runtime_cache.h` — module cache and hash-map slot-lookup types

#### Changed
- WASM compile target switched from **WASI** to bare **`wasm32`** with self-contained `printf`; eliminates WASI libc dependency in all SDK apps
- `akira_runtime.c` fully rewritten — removed all legacy OCRE glue, magic numbers, and duplicate loader paths
- Settings manager reworked to flash-backed persistence; WiFi credentials now pulled from settings at runtime

#### Fixed
- WASM out-of-bounds memory trap on ESP32-S3 during certain API calls
- App lifecycle freezes when stopping and restarting the same WASM module
- Boot crash when SD card is absent but storage API is enabled
- App install stack overflow on ESP32-S3 (increased dedicated install thread stack)
- DRAM overflow on boards without PSRAM (reduced static allocations; fallback paths added)
- BT mode conflict when BLE and BT HID are active simultaneously
- Poll stack overflow under concurrent WiFi and web server traffic

---

### Security

#### Added
- **Capability Guard model** — every native API export guarded by `akira_security_check_exec()` with `AKIRA_CAP_*` bitmask; manifests declare required capabilities
- **SHA-256 binary integrity** — mbedTLS-backed hash verified before any WASM binary is loaded into the runtime
- **Ed25519 / RSA signature verification** — `src/runtime/security/app_signing_v2.c` validates app publisher signatures via mbedTLS
- **Runtime sandbox** — `sandbox.c` / `sandbox.h` enforce syscall filtering and per-app call rate limiting
- **Security audit log** — capability violations and sandbox events written to ring buffer for post-mortem inspection
- `security.c` / `security.h` — unified capability authority (replaces the two divergent `security.h` / `capability.h` files)

#### Fixed
- Multiple redundant `#define` declarations for capability constants across headers
- `AKIRA_CAP_*` symbols were declared in both `security.h` and `capability.h` — now single source of truth

---

### Connectivity

#### Added
- **Arduino-style BLE App API** — replaces raw `bt_shell`; full 12-function surface: `ble_init`, `ble_deinit`, `ble_set_local_name`, `ble_service_create`, `ble_char_create`, `ble_service_add_char`, `ble_add_service`, `ble_set_advertised_service`, `ble_advertise`, `ble_stop_advertise`, `ble_char_write`, `ble_event_pop` and more
- **BLE GATT service pool** — multi-service, multi-characteristic layout with CCC/notify plumbing; event queue (`K_MSGQ`) for async connect/disconnect/write events
- **BT HID WASM API** — hotkey scripting and custom key bindings from WASM apps (`bt_hid_send_key`, `bt_hid_send_media`)
- **USB HID mouse support** added alongside existing keyboard (`feat(usb_hid): added mouse to usb_hid`)
- **TCP/UDP WASM network API** — zero-copy ring-buffer shared-memory channel; `net_tcp_connect`, `net_tcp_send`, `net_udp_send`, `net_recv`
- **Shared HTTP upload buffer pool** — prevents fragmentation during concurrent OTA + web-server traffic
- Matter, Thread, and AkiraMesh connectivity layer scaffolding

#### Changed
- WiFi scan (`wifi_scan` shell command) now blocks for 5 s and prints results inline instead of to a background log
- HTTP web server refactored to callback-based transport layer; stack size increased
- USB HID manager ported to the new Zephyr USB stack

#### Fixed
- BLE HID iOS compatibility — corrected HID descriptor to separate keyboard from composite device mode
- BT HID report descriptor misalignment causing dropped keys
- Web server crash on first HTTP request after boot
- OTA partition layout — NVS/LFS partitions relocated above OTA region; stale boot counter healed on upgrade
- Net buffer exhaustion under concurrent WiFi + web server: `NET_PKT RX/TX` raised 8→12, `BUF_TX` raised 16→20

---

### Hardware & Board Support

#### Added
- **AkiraConsole** custom out-of-tree ESP32-S3 board — full HWMv2 definition: `boards/akiraconsole/` with `.dts`, pinctrl `.dtsi`, `.yaml`, `_defconfig`, `board.cmake`, `board.yml`, and per-board `.conf` / `.overlay`
- **ESP32-S3 Super Mini** out-of-tree board — HWMv2-compatible definition in `boards/esp32s3_super_mini/`
- **`akira,ili9341`** out-of-tree Zephyr display driver for AkiraConsole (`feat(display): add akira,ili9341 out-of-tree driver`)
- **LR1121 transceiver** RF framework driver with `SYS_INIT` auto-init hooks (`src/drivers/rf/`)
- **SD card** support — `sd_card.c` / `sd_card.h`; direct app install from SD; WASM storage API on top
- `boards/lr1121_template.overlay` — reference overlay for LR1121 radio integration
- `boards/nrf54l15dk_nrf54l15_cpuapp` board files added to overlay set

#### Changed
- ST7789V display integrated with Zephyr native driver and dynamic capability registration
- All board `.conf` / `.overlay` files refactored for strict modularity; board-specific settings removed from root `prj.conf`
- LSM6DS3 IMU driver moved to out-of-tree with generic sensor WASM API facade

#### Fixed
- STM32 B-U585I-IOT02A boot sequence; app image now signed for MCUBoot before flash
- ESP32-S3 Super Mini board definition updated for Zephyr 4.3 HWMv2 requirements
- ESP32 DevKitC board configs cleaned up; removed duplicated symbols

---

### WASM APIs

New native functions registered into the WASM sandbox (`akira_export_api.c`):

| Domain | New Exports |
|---|---|
| GPIO / Input | `gpio_set`, `gpio_get`, `button_event_pop` |
| Timer | `timer_create`, `timer_start`, `timer_stop`, `timer_delete` |
| UART / I2C / PWM | `uart_write`, `i2c_read`, `i2c_write`, `pwm_set` |
| Display | `display_fill`, `display_draw_rect`, `display_draw_line`, `display_draw_circle`, `display_blit` |
| Sensor | `sensor_read`, `sensor_start`, `sensor_stop` |
| BLE | Full BLE App API (12 functions, see Connectivity) |
| BT HID | `bt_hid_send_key`, `bt_hid_send_media`, `bt_hid_send_consumer` |
| Network (TCP/UDP) | `net_tcp_connect`, `net_tcp_send`, `net_udp_bind`, `net_udp_send`, `net_recv`, `net_close` |
| Storage / FS | `fs_open`, `fs_read`, `fs_write`, `fs_seek`, `fs_close`, `fs_list`, `fs_stat`, `fs_remove` |
| Power | `power_sleep`, `power_deep_sleep`, `power_get_battery_level`, `power_get_charge_state` |
| RF | `rf_send`, `rf_recv`, `rf_set_channel`, `rf_get_rssi` |
| App Lifecycle | `app_start`, `app_stop`, `app_get_state` |
| IPC | `ipc_send`, `ipc_recv` (inter-app messaging ring buffer) |

---

### AkiraSDK & WASM Apps

#### Added
- **`AkiraSDK/` git submodule** — replaces `wasm_sample/`; SDK is now independently versioned at its own repository
- SDK ships 17 pre-built cross-compiled WASM apps: `hello_world`, `gpio`, `ble_led`, `compass`, `cube3d`, `display_test`, `imu_3d`, `imu_timer_test`, `inclinometer`, `logic_analyzer`, `macro_pad`, `net_echo`, `net_server`, `retro`, `storage_test`, `supervisor`, `tetris`
- **Logic Analyzer 2.0** — fully rewritten 4-channel logic analyzer app
- **Tetris** — complete playable game for AkiraConsole display
- **IMU visualization suite** — 3D IMU viewer, inclinometer, compass, and timer-based IMU test apps
- SDK `Makefile` supports bare `wasm32` cross-compilation and AOT pre-compilation targets

#### Changed
- All SDK apps ported from WASI to bare `wasm32` target with self-contained `printf`
- `AkiraSDK/docs/` content deduplicated — single source of truth; OS-level docs live only in the main repo

---

### Shell & Settings

#### Added
- `shell_rf.c` — RF shell commands: channel set, TX/RX test, RSSI readout
- `shell_display_cmds.c` — display shell commands: fill, clear, test patterns
- `shell_web.c` — web server shell commands: start, stop, status
- `shell_sys.c` — system info shell commands (migrated from legacy `akira/shell.c`)

#### Fixed
- Cold reboot issue — reboot now properly resets all subsystem state before `sys_reboot()`
- AOT mode not persisting across reboots — AOT config now saved to NVS via settings manager
- App and main stack limits raised for Retro game workload (`fix(shell,config)`)
- `build.sh` ANSI color codes not rendering in `--help` on some terminals

---

### Build System & CI/CD

#### Added
- `Dockerfile.ci` — hermetic Docker image for reproducible CI builds; Espressif toolchain pre-installed
- `.github/workflows/ci.yml` — parallel board build matrix: ESP32-S3, ESP32-C3, STM32 B-U585I, nRF54L15, Native Sim
- `.github/ISSUE_TEMPLATE/bug_report.md` and `feature_request.md`
- `.github/PULL_REQUEST_TEMPLATE.md`
- `build.sh` board auto-discovery — detects available `boards/` overlays without manual specification

#### Changed
- Zephyr SDK version bumped to **0.17.4** in CI
- Old `.github/workflows/build.yml` replaced by the new `ci.yml` matrix pipeline
- `build.sh` fully rewritten — cleaner argument parsing, better error messages, board list auto-generation

#### Fixed
- Docker `exec` environment variable passthrough for Espressif toolchain paths
- STM32 boot: app image signing step added before west flash

---

### Documentation

#### Added
- Full **GitHub Pages site** at [docs.akiraos.dev](https://docs.akiraos.dev) — Jekyll + Just the Docs theme
- Structured docs tree:
  - `docs/api-reference/` — native API reference, manifest format, error codes
  - `docs/architecture/` — AOT compilation guide, advanced connectivity, runtime internals
  - `docs/getting-started/` — quickstart, hardware setup, first WASM app
  - `docs/development/` — build options, SDK usage, contributing guide
  - `docs/hardware/` — supported boards, AkiraConsole schematics
  - `docs/platform/` — Zephyr integration, WAMR internals
  - `docs/resources/` — FAQ, glossary, performance benchmarks
- `docs/_config.yml` — Jekyll site configuration (Just the Docs, dark theme, mermaid diagrams)
- `assets/AKIRAOS-LOGO.png` and `assets/logo.png` — branding assets
- `CODE_OF_CONDUCT.md`

#### Changed
- `README.md` significantly expanded with architecture overview, board matrix, and quick-start instructions
- `QUICKSTART.md` rewritten — accurate West manifest commands, board-specific build instructions, SDK usage
- `CONTRIBUTING.md` updated with v1.4.x branch conventions and PR template guidance

#### Removed
- Flat legacy doc files: `docs/ARCHITECTURE.md`, `docs/CONNECTIVITY.md`, `docs/HARDWARE.md`, `docs/APP_DEVELOPMENT_GUIDE.md`, `docs/APP_MANAGER.md`, `docs/PENDING_FEATURES.md`, `docs/SUPPORTED_BOARDS.md`, `docs/WASM_QUICK_REFERENCE.md` — all superseded by the new structured docs site
- `docs/AkiraConsole/` and `docs/AkiraMicro/` binary assets (STL + PDF) removed from repo; hosting moved externally

---

### Performance

| Metric | Before (OCRE) | After (WAMR + AOT) |
|---|---|---|
| RAM overhead per app slot | ~120 KB | ~60 KB |
| WASM execution speed | Interpreted | Native (AOT) |
| Boot time to first app (ESP32-S3) | ~4.2 s | ~1.8 s |
| Net buffer headroom under peak WiFi | Marginal | Stable (raised pools) |

---

### CI Matrix

| Board | Build | Tested on HW |
|---|---|---|
| `native_sim` | Passing | Yes (exe) |
| `esp32s3_devkitm_esp32s3_procpu` | Passing | Yes |
| `akiraconsole_esp32s3_procpu` | Passing | Yes |
| `esp32s3_super_mini_esp32s3_procpu` | Passing | Yes |
| `b_u585i_iot02a` | Passing | Yes |
| `esp32_devkitc_procpu` | Passing | Yes |
| `esp32c3_devkitm` | Passing | Yes |

---

## [1.3.8] — "AkiraOS-v1.3.8-GL1TCH"

Legacy OCRE-based release. Superseded by v1.4.9.

---

## [1.2.3] — 2024

Initial public release with OCRE runtime, basic BLE, WiFi OTA, and LittleFS storage.
See branch `v1.2.3` for history.
