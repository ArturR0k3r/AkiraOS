---
layout: default
title: API Reference
nav_order: 4
has_children: true
permalink: /api-reference
---

# API Reference Overview

Complete reference for AkiraOS WASM application APIs.

> **Canonical reference:** The full, up-to-date API is documented in the AkiraSDK repository.  
> See [AkiraSDK/docs/API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/API_REFERENCE.md) for function signatures, constants, and examples.  
> For most apps, use `#include "akira_api.h"` (from `AkiraSDK/include/`) which provides all imports.

## Available APIs

AkiraOS provides a custom native API for WASM applications. This is **not** WASI—it's optimized for embedded systems and real-time constraints.

### Important Distinction

There are **two API layers** in AkiraOS:

1. **Native WASM Exports**: Low-level natives registered via `"env"` module in `src/api/akira_export_api.c`
   - All categories are gated by Kconfig (`CONFIG_DISPLAY`, `CONFIG_AKIRA_WASM_BLE`, etc.)
   - Minimal overhead, real-time suitable
   - Documented in [Native API Reference](native-api.md)

2. **SDK Wrapper API**: High-level convenience library (`AkiraSDK/include/akira_api.h`)
   - Provides `printf()`, color constants, `SENSOR_CHAN_*` defines, and thin wrappers
   - See [AkiraSDK API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/API_REFERENCE.md)

**Most developers use the SDK API** (via `#include "akira_api.h"`), which automatically handles native imports.

### API Categories

| Category | Key Functions | Kconfig Guard |
|----------|---------------|---------------|
| [Logging / Timing](#logging-api) | `printf_native`, `delay` | `CONFIG_AKIRA_WASM_API` |
| [Display](#display-api) | `display_clear`, `display_text`, `display_rect`, `display_flush` | `CONFIG_DISPLAY` |
| [GPIO](#gpio-api) | `gpio_configure`, `gpio_read`, `gpio_write` | `CONFIG_GPIO` |
| [Sensor](#sensor-api) | `sensor_read` | `CONFIG_SENSOR` |
| [RF](#rf-api) | `rf_send`, `rf_set_frequency`, `rf_set_power`, `rf_get_rssi` | `CONFIG_AKIRA_RF_FRAMEWORK` |
| [Storage](#storage-api) | `storage_open`, `storage_read`, `storage_write`, `storage_close` | `CONFIG_AKIRA_WASM_STORAGE` |
| [Net / Sockets](#net-api) | `net_open`, `net_connect`, `net_bind`, `net_tx_bind`, `net_rx_bind` | `CONFIG_AKIRA_WASM_NET` |
| [BLE](#ble-api) | `ble_init`, `ble_service_create`, `ble_char_write`, `ble_event_pop` | `CONFIG_AKIRA_WASM_BLE` |
| [HID](#hid-api) | `hid_key_press`, `hid_type_string`, `hid_mouse_move`, `hid_gamepad_*` | `CONFIG_AKIRA_WASM_HID` |
| [Timer](#timer-api) | `timer_create`, `timer_start`, `timer_elapsed`, `timer_free` | `CONFIG_AKIRA_WASM_TIMER` |
| [UART](#uart-api) | `uart_open`, `uart_write`, `uart_read`, `uart_close` | `CONFIG_AKIRA_WASM_UART` |
| [I2C](#i2c-api) | `i2c_write_reg`, `i2c_read_reg` | `CONFIG_AKIRA_WASM_I2C` |
| [PWM](#pwm-api) | `pwm_set`, `pwm_disable` | `CONFIG_AKIRA_WASM_PWM` |
| [Power](#power-api) | `power_get_battery_level`, `power_set_mode`, `power_wake_on_gpio` | `CONFIG_AKIRA_WASM_POWER` |
| [Memory](#memory-api) | `mem_alloc`, `mem_free` | `CONFIG_AKIRA_WASM_MEMORY` |
| [IPC Pub/Sub](#ipc-api) | `msg_subscribe`, `msg_publish`, `msg_recv` | `CONFIG_AKIRA_WASM_IPC` |
| [App Lifecycle](#lifecycle-api) | `app_start`, `app_stop`, `app_switch`, `app_get_self_name` | `CONFIG_AKIRA_WASM_LIFECYCLE` |

## Import Module

All native functions are imported from the `"env"` module (registered via WAMR's `wasm_runtime_register_natives`):

```c
// SDK header (AkiraSDK/include/akira_api.h) declares all imports as extern:
extern int display_clear(uint32_t color);
extern int sensor_read(int32_t channel);
// ... etc.
```

**With the SDK header you do not need manual `__attribute__((import_module))` annotations.**

## Logging API

### `printf_native` / `printf`
Send a message to the AkiraOS host console.

```c
extern int printf_native(const char *message);  // raw native export
void printf(const char *fmt, ...);              // SDK wrapper (supports %d, %s)
```

**No capability required.** The SDK's `printf()` handles formatting internally; newlines are stripped by the host logger.

### `delay`
Yield execution for the given number of **microseconds**.

```c
extern int delay(uint32_t microseconds);
```

---

## Display API

> Full display API (20+ functions) documented in [AkiraSDK API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/API_REFERENCE.md#display-api).

### `display_clear`
Clear the entire screen to a specific color.

```c
extern int display_clear(uint32_t color);
```

**Parameters:**
- `color`: RGB565 16-bit color (e.g., `COLOR_RED` = 0xF800)

**Returns:** 0 on success, `-EACCES` if missing `CAP_DISPLAY_WRITE`

**Capability Required:** `CAP_DISPLAY_WRITE`

---

### `display_pixel` / `display_rect` / `display_text` / `display_text_large` / `display_flush`

```c
extern int display_pixel(int32_t x, int32_t y, uint32_t color);
extern int display_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
extern int display_text(int32_t x, int32_t y, const char *text, uint32_t color);
extern int display_text_large(int32_t x, int32_t y, const char *text, uint32_t color);
extern int display_flush(void);
```

All require `CAP_DISPLAY_WRITE`.

---

## Input API

### `input_read_buttons`
Read digital button states.

> **Note:** When using `#include "akira_api.h"` from the AkiraSDK, manual `__attribute__((import_module))` annotations are not required. The example below shows the low-level import syntax for reference.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("input_read_buttons")))
extern uint32_t akira_input_read_buttons();
```

**Returns:** Bitmask of button states (1 = pressed)

**Capability Required:** `CAP_INPUT_READ`

**Button Mapping:**
- Bit 0: Button A
- Bit 1: Button B
- Bit 2: Button C
- Bit 3: Button D

---

### `input_read_touch`
Read touchscreen coordinates.

```c
extern int akira_input_read_touch(uint32_t *x, uint32_t *y);
```

**Returns:** 0 if touched, -1 if not

> **Note:** `input_read_buttons` and `input_read_touch` are not native exports in the current runtime. Check the AkiraSDK docs for the current input API.

---

## Sensor API

### `sensor_read`
Read sensor value by channel.

> **Note:** When using `#include "akira_api.h"` from the AkiraSDK, manual `__attribute__((import_module))` annotations are not required. The example below shows the low-level import syntax for reference.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("sensor_read")))
extern int sensor_read(int32_t channel);
```

**Parameters:**
- `channel`: Zephyr `enum sensor_channel` integer — use `SENSOR_CHAN_*` constants from `akira_api.h`

**Returns:** Reading scaled ×1000 on success (divide by 1000.0 to get physical value), or `AKIRA_SENSOR_ERROR` (`INT32_MIN`) on any error.

**Capability Required:** `CAP_SENSOR_READ`

**Common Channel IDs** (Zephyr `enum sensor_channel` values):
- `SENSOR_CHAN_ACCEL_X` = 0, `ACCEL_Y` = 1, `ACCEL_Z` = 2
- `SENSOR_CHAN_GYRO_X` = 4, `GYRO_Y` = 5, `GYRO_Z` = 6
- `SENSOR_CHAN_AMBIENT_TEMP` = 13
- `SENSOR_CHAN_PRESS` = 14
- `SENSOR_CHAN_HUMIDITY` = 16

> For the full list of `SENSOR_CHAN_*` constants see `AkiraSDK/include/akira_api.h`.

---

## RF/Network API

> **Work In Progress:** The RF module is implemented but still under active development. Use with caution in production environments.

> See [AkiraSDK API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/API_REFERENCE.md#rf-api) for full RF and network API.

### `rf_send`
Send data via RF interface.

> **Note:** When using `#include "akira_api.h"` from the AkiraSDK, manual `__attribute__((import_module))` annotations are not required. The example below shows the low-level import syntax for reference.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("rf_send")))
extern int rf_send(const uint8_t *data, uint32_t len);
```

**Capability Required:** `CAP_RF_TRANSCEIVE`

**Only available when `CONFIG_AKIRA_RF_FRAMEWORK=y`.**

---

## Net API

> Socket-style network API. See [AkiraSDK API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/API_REFERENCE.md#network-api) for full reference.

**Available when `CONFIG_AKIRA_WASM_NET=y`:** `net_open`, `net_connect`, `net_bind`, `net_listen`, `net_close`, `net_tx_bind`, `net_rx_bind`, `net_tx_flush`, `net_event_pop`.

---

## Storage API

Storage functions operate within a per-app sandbox. Paths are relative to the app's data directory. Path traversal (`..`) is rejected with `-EACCES`.

### `storage_open`
Open a file in the app sandbox.

```c
extern int storage_open(const char *path, int flags);
```

**Capability Required:** `storage.read` (read flags) or `storage.write` (write flags)

**Flags:** `STORAGE_O_READ` (0), `STORAGE_O_WRITE` (1), `STORAGE_O_APPEND` (2), `STORAGE_O_RDWR` (3)

---

### `storage_read` / `storage_write` / `storage_close`

```c
extern int  storage_read(int fd, void *buf, int len);
extern int  storage_write(int fd, const void *buf, int len);
extern void storage_close(int fd);
```

---

### `storage_delete`
Delete a file from the app sandbox.

```c
extern int storage_delete(const char *path);
```

**Capability Required:** `storage.write`

---

### `storage_list`
List files in sandbox directory.

```c
extern int storage_list(const char *path, char *buf, int len);
```

**Returns:** Newline-separated list of filenames, NUL-terminated.

---

## Logging API

> Logging is documented above under [Logging / Timing](#logging-api). Use `printf_native` / `printf()` from the SDK.

---

## Time API

> `time_ms()` and `sleep_ms()` are not native exports in the current runtime — use `delay(microseconds)` (Logging/Timing section above) or the Timer API (`timer_create`, `timer_elapsed`).

---

## Error Codes

| Code | Constant | Meaning |
|------|----------|---------|
| 0 | OK | Success |
| -1 | EPERM | Capability check failed |
| -2 | ENOENT | File not found |
| -5 | EIO | I/O error |
| -12 | ENOMEM | Out of memory |
| -22 | EINVAL | Invalid argument |

See [Error Codes Reference](error-codes.md) for the full table including domain-specific (`AKIRA_ERR_*`) codes.

---

## Complete Example

```c
#include "akira_api.h"  // from AkiraSDK/include/

__attribute__((export_name("_start")))
void app_main() {
    printf("App starting");

    // Clear screen to blue (RGB565)
    display_clear(COLOR_BLUE);

    // Read temperature — sensor_read returns value x1000
    int raw = sensor_read(SENSOR_CHAN_AMBIENT_TEMP);
    if (raw != AKIRA_SENSOR_ERROR) {
        printf("Temp: %d milli-C", raw);
    }

    // Flush framebuffer
    display_flush();

    // Yield 1 second
    delay(1000000);
}
```

---

## Type Reference

### WASM Function Signatures

When registering native functions in WAMR, these signatures are used:

- `"($)v"` - String parameter, void return
- `"(i)i"` - Int parameter, int return
- `"(ii)i"` - Two int params, int return
- `"(*)i"` - Pointer param, int return
- `"(*i)i"` - Pointer and int, int return
- `"()i"` - No params, int return

---

## Related Documentation

- [Manifest Format](manifest-format.md) - Capability declarations
- [Building Apps](../development/building-apps.md) - WASM compilation
- [First App Tutorial](../getting-started/first-app.md) - Hello World
- [Security Model](../architecture/security.md) - Capability system

---

*Last updated: 2026-09-14 (AkiraOS v1.6.4)*
