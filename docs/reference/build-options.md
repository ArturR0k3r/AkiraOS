---
layout: default
title: Build Options Reference
parent: Development
nav_order: 10
---

# AkiraOS Build Options Reference

AkiraOS uses Zephyr's Kconfig system for build-time configuration. These options allow you to tailor the firmware's storage, features, hardware capabilities, and limitations to fit resource-constrained boards.

Below is a subset of the critical AkiraOS features (`CONFIG_AKIRA_*`) and runtime properties. Add these to your `prj.conf` or board-specific `.conf` file.

### OS & Core Settings

| Config Flag | Default | Description |
|---|---|---|
| `CONFIG_AKIRA_OS` | `y` | Enable overall AkiraOS features |
| `CONFIG_AKIRA_LOG_LEVEL` | `3` | Log level: `0`=OFF, `1`=ERR, `2`=WRN, `3`=INF, `4`=DBG |
| `CONFIG_AKIRA_TERMINAL_MODE` | `n` | Enable hacker terminal with network tools |
| `CONFIG_AKIRA_CYBERPUNK_THEME`| `n` | Enable cyberpunk UI theme (LVGL) |

### Runtime & WebAssembly 

| Config Flag | Default | Description |
|---|---|---|
| `CONFIG_MAX_CONTAINERS` | `8` | Max number of simultaneously loaded WASM modules (not the same as running apps). |
| `CONFIG_AKIRA_APP_MAX_INSTALLED` | `8` | Max apps stored on flash. |
| `CONFIG_AKIRA_APP_MAX_RUNNING` | `2` | Max concurrently running WASM apps. Increase only if memory allows. |
| `CONFIG_AKIRA_WASM_RUNTIME` | `n`* | Enable WebAssembly runtime (WAMR). Set to `y` in `prj.conf` for all AkiraOS builds. |
| `CONFIG_WAMR_AOT_SUPPORT` | `n` | Enable WASM AOT compilation. Recommended for CPU-heavy tasks but costs flash space. Only `akiraconsole` board enables this by default. |
| `CONFIG_WAMR_HEAP_SIZE` | `262144`| WAMR heap size in bytes (Kconfig default). Overridden per-board: ESP32-S3 uses 1 MB, ESP32 uses 32 KB. |
| `CONFIG_AKIRA_WASM_APP_STACK_SIZE`| `8192`| Stack size per WASM app thread (SRAM only). |
| `CONFIG_AKIRA_APP_MANAGER` | `y` | Enable App Manager for lifecycle (install/run WASM apps). |

### Connectivity & Network Options

| Config Flag | Default | Description |
|---|---|---|
| `CONFIG_AKIRA_WIFI` | `y`* | Enable WiFi subsystem if hardware supports it. |
| `CONFIG_AKIRA_HTTP_SERVER` | `y` | Enable HTTP server for REST API and app uploads. |
| `CONFIG_AKIRA_WS_CLIENT` | `n` | Enable WebSocket client. |
| `CONFIG_AKIRA_CLOUD_CLIENT` | `y` | Enable unified cloud client. |
| `CONFIG_AKIRA_OTA` | `y` | Enable OTA firmware updates orchestration. |
| `CONFIG_AKIRA_MESH` | `n` | Enable AkiraMesh custom protocol routing. |
| `CONFIG_AKIRA_THREAD` | `n` | Enable OpenThread mesh (requires 802.15.4 chip like nRF). |

### Security & Privilege 

| Config Flag | Default | Description |
|---|---|---|
| `CONFIG_AKIRA_CAPABILITY_SYSTEM`| `y` | Enable capability-based security. Mandatory for safe WASM execution. |
| `CONFIG_AKIRA_APP_SIGNING` | `n` | Require WASM app signature verification (RSA/Ed25519). |
| `CONFIG_AKIRA_SANDBOX_RATE_LIMITING`| `y` | Throttle aggressive syscall usage by apps to prevent DOS. |
| `CONFIG_AKIRA_RESOURCE_MANAGER` | `y` | Enable resource (RAM/Flash) quota management per app. |

### API / Capabilities Exposure

Toggle these flags to export particular native APIs into the WASM sandboxes. If disabled, apps requiring them will fail capability checks.

| Config Flag | Default | API Domain |
|---|---|---|
| `CONFIG_AKIRA_WASM_API` | `y` | Enable baseline native API exports. |
| `CONFIG_AKIRA_WASM_POWER` | `y` | Power sensing API. |
| `CONFIG_AKIRA_WASM_POWER_CONTROL`| `n` | Restricted power control API (e.g., Deep Sleep commands). |
| `CONFIG_AKIRA_WASM_HID` | `n` | Export Human Interface Device API to WASM. |
| `CONFIG_AKIRA_WASM_LIFECYCLE` | `y` | Export app manipulation API (`app_start`, `app_stop`). |
| `CONFIG_AKIRA_WASM_IPC` | `y` | Export Pub/Sub IPC API. |
| `CONFIG_AKIRA_WASM_ADC` | `n` | Export ADC API (`adc_read`, `adc_read_mv`). Requires `CONFIG_ADC=y`. |
| `CONFIG_AKIRA_WASM_ADC_MAX_CHANNELS`| `4` | Number of ADC channels accessible to WASM apps. |
| `CONFIG_AKIRA_WASM_ADC_RESOLUTION`| `12` | ADC bit resolution (bits). |
| `CONFIG_AKIRA_WASM_ADC_VREF_MV`| `3300` | ADC reference voltage in millivolts for `adc_read_mv()`. |
| `CONFIG_AKIRA_WDT` | `n` | Enable AkiraOS system watchdog manager (auto-feed worker). Requires `CONFIG_WATCHDOG=y`. |
| `CONFIG_AKIRA_WDT_TIMEOUT_MS` | `30000` | Watchdog hardware timeout in milliseconds. |
| `CONFIG_AKIRA_WDT_FEED_INTERVAL_MS`| `10000`| Auto-feed interval in milliseconds (must be < timeout). |
| `CONFIG_AKIRA_WASM_WDT` | `n` | Export `wdt_pet()` to WASM apps. Requires `CONFIG_AKIRA_WDT=y`. |

### Storage & Settings

| Config Flag | Default | Description |
|---|---|---|
| `CONFIG_AKIRA_SETTINGS` | `y` | Enable Settings Manager (NVS key-value store). |
| `CONFIG_AKIRA_SD_CARD` | `n` | Mount SD Card filesystem integration. |

For detailed constraints (like SRAM/PSRAM boundaries), see the [Architecture Overview](../architecture/system-overview.md).