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
| `CONFIG_AKIRA_APP_SIGNING` | `y` | Enable WASM app signature verification (Ed25519 / Dilithium-2). |
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
| `CONFIG_AKIRA_WASM_LIFECYCLE` | `n` | Export app manipulation API (`app_start`, `app_stop`). |
| `CONFIG_AKIRA_WASM_IPC` | `n` | Export Pub/Sub IPC API. |
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

### Security Hardening (1.6.x)

| Config Flag | Default | Description |
|---|---|---|
| `CONFIG_AKIRA_REQUIRE_SIGNED_APPS` | `n` | Fail closed when no platform signature verifier is linked. Set `y` for production. |
| `CONFIG_AKIRA_ALLOW_UNSIGNED_APPS` | `n` | Allow unsigned WASM apps (development only). |
| `CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK` | `0xFFFFFFFFFFFFFFFF` | Capability allow-mask applied to unsigned/unattested apps. Narrow this in production. |
| `CONFIG_AKIRA_OTA_REQUIRE_HASH` | `y` | Require a verified SHA-256 for every OTA image before it is marked bootable. |
| `CONFIG_AKIRA_SETTINGS_ENCRYPTION` | `n` | Transparent AES-256 encryption of the settings store. |
| `CONFIG_AKIRA_SETTINGS_PER_DEVICE_KEY` | `y` | Derive the settings key from the hardware unique ID instead of a compile-time constant. |
| `CONFIG_AKIRA_SE050` | `y` if `nxp,se050` in DT | NXP SE050 secure element driver. |
| `CONFIG_AKIRA_SE050_SCP03` | `n` | GlobalPlatform SCP03 authenticated/encrypted channel to the SE050. |
| `CONFIG_AKIRA_BOOT_GUARD` | `n` | Software boot counter / rollback guard. |
| `CONFIG_AKIRA_RELEASE_BUILD` | `n` | Fail the build while development-only security settings are active: HTTP no-auth, empty or example upload token, direct upload endpoint, unsigned apps, unsigned or dev-key MCUboot images. With `n` they are printed as a warning. |

### HTTP Server (1.6.x)

| Config Flag | Default | Description |
|---|---|---|
| `CONFIG_AKIRA_HTTP_STATIC_FILES` | `n` | Serve static files from the filesystem, with extension→Content-Type mapping and path-traversal protection. |
| `CONFIG_AKIRA_HTTP_WEBSOCKET` | `y` | RFC 6455 WebSocket server support. |
| `CONFIG_AKIRA_HTTP_NO_AUTH` | `y` | Skip Bearer token checks on all HTTP endpoints. Set `n` in production. |
| `CONFIG_AKIRA_HTTP_UPLOAD_TOKEN` | `""` | Static Bearer token for `POST /upload` and `POST /api/apps/install`. |
| `CONFIG_AKIRA_HTTP_DEV_UPLOAD` | `y` | Register the direct WASM upload endpoint. Disable in production. |

### Connectivity & Peripherals (1.6.x)

| Config Flag | Default | Description |
|---|---|---|
| `CONFIG_AKIRA_MQTT` | `n` | MQTT client with Home Assistant discovery. |
| `CONFIG_AKIRA_BT_COMPANION` | `n` | BLE companion service (phone app command bridge). |
| `CONFIG_AKIRA_SD_HOTPLUG` | `n` | SD card hotplug detection via the `akira,sd-detect` DT node. |
| `CONFIG_AKIRA_COAP_BLOCK_TRANSFER` | `y` | CoAP block-wise transfer (RFC 7959). |
| `CONFIG_AKIRA_RADIO_802154_PROMISC_RX` | `y` if promiscuous mode | Raw 802.15.4 RX capture with real per-frame LQI/RSSI. |
| `CONFIG_AKIRA_LVGL_INPUT_ZEPHYR` | `y` if `INPUT` | Bridge Zephyr input events (gpio-keys / D-pad / gamepad / touch) into LVGL. |
| `CONFIG_AKIRA_HID` | `n` | HID subsystem. Pair with `CONFIG_AKIRA_HID_MODE_KB_MOUSE` or `CONFIG_AKIRA_HID_MODE_GAMEPAD`. |
| `CONFIG_AKIRA_OTA_DELTA` | `n` | Delta (binary-patch) OTA updates. |

### WASM API Exports (1.6.x)

These register the corresponding native symbols. See the
[Native API symbol index](../api-reference/native-api.md#complete-symbol-index).

| Config Flag | Default | API Domain |
|---|---|---|
| `CONFIG_AKIRA_WASM_MESH` | `y` | `mesh_*` — AkiraMesh. Requires `CONFIG_AKIRA_MESH=y` to do anything. |
| `CONFIG_AKIRA_WASM_MQTT` | `n` | `mqtt_*` and `ha_light_*`. **Required, or MQTT app imports fail to link.** |
| `CONFIG_AKIRA_WASM_MATTER` | `n` | `matter_*`. |
| `CONFIG_AKIRA_WASM_AIINFER` | `y` | `aiinfer_*`. Requires `CONFIG_AKIRA_AIINFER=y` (default `n`). |
| `CONFIG_AKIRA_WASM_CRYPTO` | — | `crypto_*`. Note there is no `crypto_ed25519_verify`. |
| `CONFIG_AKIRA_WASM_FS` | — | `fs_*` filesystem access. |
| `CONFIG_AKIRA_WASM_SETTINGS` | — | `settings_get` / `settings_set` / `settings_delete`. |

> **AkiraSync (`CONFIG_AKIRA_SYNC_*`) is not buildable.** The options exist, but
> `src/connectivity/sync/` is not referenced by `CMakeLists.txt` and no `sync_*` native is
> registered. Enabling them has no effect.

There are **306** `CONFIG_AKIRA_*` symbols in total; the tables above cover the ones most
often changed. `Kconfig` at the repository root is the complete, authoritative list.

For detailed constraints (like SRAM/PSRAM boundaries), see the [Architecture Overview](../architecture/system-overview.md).

---

*Last updated: 2026-09-14 (AkiraOS v1.6.4)*
