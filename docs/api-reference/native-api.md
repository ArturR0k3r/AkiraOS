---
layout: default
title: Native API
parent: API Reference
nav_order: 1
---

# Native API Reference

Complete reference for all AkiraOS native functions callable from WASM.

> **Note:** This is a custom API, **not WASI**. It's designed specifically for embedded systems and real-time constraints.
> 
> For the full, up-to-date function list with examples, see the canonical **[AkiraSDK API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/API_REFERENCE.md)**.  
> Use `#include "akira_api.h"` from `AkiraSDK/include/` — it declares all exports, color constants, and `SENSOR_CHAN_*` defines.

## Import Declaration

All functions are registered in the `"env"` module by `akira_register_native_apis()` in `src/api/akira_export_api.c`. Using the SDK header you do not need manual import attributes:

```c
// With AkiraSDK header — no attributes needed:
#include "akira_api.h"
display_clear(COLOR_BLUE);

// Manual import (without SDK header):
__attribute__((import_module("env")))
__attribute__((import_name("function_name")))
extern return_type function_name(parameters);
```

---

## Display Functions

### `display_clear(color)`

Clear the entire display to a solid color.

```c
extern int display_clear(uint32_t color);
```

**Parameters:**
- `color` (uint32_t): RGB565 16-bit color

**Returns:**
- `0`: Success
- `-EACCES`: Permission denied (missing `CAP_DISPLAY_WRITE`)

**Capability Required:** `CAP_DISPLAY_WRITE`

> **Note:** The underlying primitive always returns 0; the capability check is the only failure path for this function.

**Example:**
```c
display_clear(COLOR_BLACK);
display_clear(COLOR_RED);
display_clear(0x001F);     // blue in RGB565
```

---

### `display_pixel(x, y, color)`

Draw a single pixel at specified coordinates.

```c
extern int display_pixel(int32_t x, int32_t y, uint32_t color);
```

**Returns:** `0` on success, `-EACCES` if missing `CAP_DISPLAY_WRITE`

**Capability Required:** `CAP_DISPLAY_WRITE`

**Example:**
```c
// Draw a red pixel at (100, 50)
display_pixel(100, 50, COLOR_RED);
```

---

### `display_rect(x, y, width, height, color)`

Draw a filled rectangle.

```c
extern int display_rect(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color);
```

**Capability Required:** `CAP_DISPLAY_WRITE`

---

### `display_text(x, y, text, color)` / `display_text_large(…)`

Render text string at specified position (small 7×10 font / large 11×18 font).

```c
extern int display_text(int32_t x, int32_t y, const char *text, uint32_t color);
extern int display_text_large(int32_t x, int32_t y, const char *text, uint32_t color);
```

**Capability Required:** `CAP_DISPLAY_WRITE`

---

### `display_flush()`

Flush framebuffer to physical display. An auto-flush fires 50 ms after the last draw call, but calling this explicitly gives smoother animation.

```c
extern int display_flush(void);
```

> Full display API (20+ functions: `display_line`, `display_circle`, `display_bitmap`, `display_progress_bar`, `display_rounded_rect`, etc.) — see [AkiraSDK API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/API_REFERENCE.md#display-api).

---

## Input Functions

> **There is no button input native API.** The runtime registers no `input_*` symbol.
> `AKIRA_CAP_INPUT_READ` (bit 1) and `AKIRA_CAP_INPUT_WRITE` (bit 2) remain defined and
> reserved, but nothing consumes them today.
>
> Apps that need button state should use the LVGL input path
> (`CONFIG_AKIRA_LVGL_INPUT_ZEPHYR`), which bridges Zephyr input events — gpio-keys, D-pad,
> gamepad and touch — into the display driver.

---

## Sensor Functions

### `sensor_read(channel)`

Read sensor value by Zephyr channel ID.

```c
extern int sensor_read(int32_t channel);
```

**Parameters:**
- `channel`: Zephyr `enum sensor_channel` integer — use `SENSOR_CHAN_*` constants from `akira_api.h`

**Returns:**
- Reading scaled ×1000 on success (divide by 1000.0 to recover physical value)
- `AKIRA_SENSOR_ERROR` (`INT32_MIN`) on any error

**Capability Required:** `CAP_SENSOR_READ`

**Sensor Channel IDs** (Zephyr `enum sensor_channel`):
| Constant | Value | Sensor | Unit |
|----------|-------|--------|------|
| `SENSOR_CHAN_ACCEL_X` | 0 | Accelerometer X | m/s² |
| `SENSOR_CHAN_ACCEL_Y` | 1 | Accelerometer Y | m/s² |
| `SENSOR_CHAN_ACCEL_Z` | 2 | Accelerometer Z | m/s² |
| `SENSOR_CHAN_GYRO_X` | 4 | Gyroscope X | rad/s |
| `SENSOR_CHAN_GYRO_Y` | 5 | Gyroscope Y | rad/s |
| `SENSOR_CHAN_GYRO_Z` | 6 | Gyroscope Z | rad/s |
| `SENSOR_CHAN_MAGN_X` | 8 | Magnetometer X | Gauss |
| `SENSOR_CHAN_MAGN_Y` | 9 | Magnetometer Y | Gauss |
| `SENSOR_CHAN_MAGN_Z` | 10 | Magnetometer Z | Gauss |
| `SENSOR_CHAN_AMBIENT_TEMP` | 13 | Temperature | °C |
| `SENSOR_CHAN_PRESS` | 14 | Pressure | kPa |
| `SENSOR_CHAN_HUMIDITY` | 16 | Relative humidity | % |
| `SENSOR_CHAN_ALTITUDE` | 23 | Altitude | m |
| `SENSOR_CHAN_VOLTAGE` | 33 | Voltage | V |
| `SENSOR_CHAN_CURRENT` | 35 | Current | A |

**Example:**
```c
int raw = sensor_read(SENSOR_CHAN_AMBIENT_TEMP);
if (raw != AKIRA_SENSOR_ERROR) {
    // raw = temperature * 1000, e.g. 24500 = 24.5 °C
    printf("Temp: %d milli-C", raw);
}
```

---

### `sensor_list(buffer, max_count)`

Get list of available sensors.

```c
extern int sensor_list(uint32_t *buffer, uint32_t max_count);
```

**Returns:** Number of sensors written to buffer

---

## RF/Network Functions

> **Work In Progress:** The RF module is implemented but still under active development. Use with caution in production environments.

> **Available only when `CONFIG_AKIRA_RF_FRAMEWORK=y`.**  
> See [AkiraSDK API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/API_REFERENCE.md#rf-api) for full RF reference.

### `rf_send(data, length)`

Send data via active RF interface.

```c
extern int rf_send(const uint8_t *data, uint32_t length);
```

**Returns:**
- `>= 0`: Bytes sent
- `-EPERM`: Missing `CAP_RF_TRANSCEIVE`
- `-EINVAL`: Zero-length payload
- `-ENOSYS`: RF framework not compiled in

**Capability Required:** `CAP_RF_TRANSCEIVE`

**Example:**
```c
const char *message = "Hello, RF!";
int sent = rf_send((uint8_t*)message, strlen(message));
if (sent < 0) { printf("Send failed: %d", sent); }
```

**Additional exports:** `rf_set_frequency`, `rf_set_power`, `rf_get_rssi`.

---

## Storage Functions

Storage functions operate within a per-app sandbox directory. All paths are relative to the app's data directory. Path traversal (`..`) is rejected with `-EACCES`.

**Capability Required:** `storage.read` for reading/listing; `storage.write` for writing/deleting.

### `storage_open(path, flags)`

Open a file in the app sandbox.

```c
extern int storage_open(const char *path, int flags);
```

**Parameters:**
- `path`: Relative path within app sandbox (e.g. `"log.txt"`)
- `flags`: `STORAGE_O_READ` (0), `STORAGE_O_WRITE` (1), `STORAGE_O_APPEND` (2), `STORAGE_O_RDWR` (3)

**Returns:** Non-negative file descriptor on success, negative errno on error.

---

### `storage_read(fd, buf, len)`

Read bytes from an open file.

```c
extern int storage_read(int fd, void *buf, int len);
```

**Returns:** Bytes read (0 = EOF), negative errno on error.

---

### `storage_write(fd, buf, len)`

Write bytes to an open file.

```c
extern int storage_write(int fd, const void *buf, int len);
```

**Returns:** Bytes written on success, negative errno on error.

---

### `storage_close(fd)`

Close a file descriptor.

```c
extern void storage_close(int fd);
```

---

### `storage_delete(path)`

Delete a file from the app sandbox.

```c
extern int storage_delete(const char *path);
```

**Capability Required:** `storage.write`

---

### `storage_list(path, buf, len)`

List files in the app sandbox directory.

```c
extern int storage_list(const char *path, char *buf, int len);
```

---

## Logging Functions

### `printf_native(message)`

Send a pre-formatted, null-terminated string to the host logger.

```c
extern int printf_native(const char *message);
```

**No capability required.** Called internally by the SDK's `printf()` wrapper.

**Example:**
```c
printf_native("Processing started");
// Or use the SDK wrapper:
printf("Temp: %d milli-C", raw);
```

> **Note:** `log()`, `log_info()`, `log_debug()`, and `log_error()` are not native exports. Use `printf_native` directly or the SDK's `printf()`.

---

## Time Functions

### `delay(microseconds)`

Yield execution for specified duration.

```c
extern int delay(uint32_t microseconds);
```

> **Note:** `time_ms()` and `sleep_ms()` are not native exports. Use `delay()` for yielding. For elapsed time, use the Timer API (`timer_create`, `timer_elapsed`).

---

## Error Codes

Standard POSIX error codes (negative values)

See [Error Codes Reference](error-codes.md) for domain-specific `AKIRA_ERR_*` codes.

---

## Performance Characteristics

| API Category | Call Overhead | Notes |
|--------------|---------------|-------|
| Display | ~60ns | Inline cap check + HAL |
| Input | ~60ns | Direct register read |
| Sensors | ~500μs | I2C transaction time |
| RF | ~10ms | Network stack overhead |
| File System | ~10ms | Flash access time |
| Logging | ~100μs | UART output |
| Time | ~20ns | Register read |

---

## ADC Functions

> **Available only when `CONFIG_AKIRA_WASM_ADC=y`.**  
> Requires `CONFIG_ADC=y` and an `akira-adc` DTS alias (or the first `zephyr,adc` device).

### `adc_read(channel)`

Read a raw ADC sample from the specified channel.

```c
extern int adc_read(int32_t channel);
```

**Parameters:**
- `channel`: ADC channel index (`0` … `CONFIG_AKIRA_WASM_ADC_MAX_CHANNELS - 1`)

**Returns:**
- Raw 12-bit (or configured resolution) sample value on success
- `-EPERM`: Missing `adc` capability
- `-ENODEV`: ADC hardware not ready
- `-EINVAL`: Channel out of range

**Capability Required:** `adc`

**Example:**
```c
int raw = adc_read(0);
if (raw >= 0) {
    printf("ADC ch0 raw: %d", raw);
}
```

---

### `adc_read_mv(channel)`

Read an ADC channel and convert the sample to millivolts.

```c
extern int adc_read_mv(int32_t channel);
```

**Parameters:**
- `channel`: ADC channel index

**Returns:**
- Voltage in millivolts on success
- Negative errno on error (same codes as `adc_read`)

**Capability Required:** `adc`

Conversion uses `CONFIG_AKIRA_WASM_ADC_VREF_MV` and `CONFIG_AKIRA_WASM_ADC_RESOLUTION`.

**Example:**
```c
int mv = adc_read_mv(0);
if (mv >= 0) {
    printf("Battery: %d mV", mv);
}
```

---

## Watchdog Functions

> **Available only when `CONFIG_AKIRA_WASM_WDT=y`.**  
> Requires `CONFIG_WATCHDOG=y` and `CONFIG_AKIRA_WDT=y`.

### `wdt_pet()`

Manually feed the system watchdog, signalling that the app is still alive.

```c
extern int wdt_pet(void);
```

**Returns:**
- `0`: Success
- `-EPERM`: Missing `wdt` capability
- `-ENODEV`: Watchdog not active

**Capability Required:** `wdt`

The AkiraOS watchdog is auto-fed every `CONFIG_AKIRA_WDT_FEED_INTERVAL_MS` ms by an internal work queue. Call `wdt_pet()` from a long-running computation to supplement the automatic feeding when the main thread may be busy for longer than that interval.

**Example:**
```c
for (int i = 0; i < 1000; i++) {
    heavy_processing_step(i);
    if (i % 100 == 0) {
        wdt_pet();   // keep watchdog happy during heavy work
    }
}
```

---

## Complete Symbol Index

Every native symbol the runtime registers into the WASM `env` module, generated from
[`src/api/akira_export_api.c`](https://github.com/ArturR0k3r/AkiraOS/blob/main/src/api/akira_export_api.c).
**201 symbols across 30 subsystems.** The sections above document the most commonly used
ones in prose; this index is the authoritative, exhaustive list.

A symbol is only present in a build when its gating Kconfig option is enabled, and calling
it still requires the listed capability in the app manifest.

Signature notation follows WAMR: `i32`/`i64`/`f32`/`f64` are value types, `string` is a
NUL-terminated string in linear memory, `ptr` is a linear-memory pointer, and `len` is the
buffer length that pairs with the preceding `ptr`.

### Core

Gated by `CONFIG_AKIRA_WASM_API`. Capability: none.

| Symbol | Signature |
|--------|-----------|
| `delay` | `(i32) -> i32` |
| `printf_native` | `(string) -> i32` |

### Display

Gated by `CONFIG_DISPLAY`. Capability: `display.write`.

| Symbol | Signature |
|--------|-----------|
| `display_bitmap` | `(i32, i32, i32, i32, ptr, len) -> i32` |
| `display_bitmap_transparent` | `(i32, i32, i32, i32, ptr, len, i32) -> i32` |
| `display_circle` | `(i32, i32, i32, i32) -> i32` |
| `display_circle_fill` | `(i32, i32, i32, i32) -> i32` |
| `display_clear` | `(i32) -> i32` |
| `display_flush` | `() -> i32` |
| `display_get_size` | `(ptr, ptr) -> i32` |
| `display_hline` | `(i32, i32, i32, i32) -> i32` |
| `display_line` | `(i32, i32, i32, i32, i32) -> i32` |
| `display_number` | `(i32, i32, i32, i32) -> i32` |
| `display_pixel` | `(i32, i32, i32) -> i32` |
| `display_progress_bar` | `(i32, i32, i32, i32, i32, i32, i32, i32) -> i32` |
| `display_raw_write` | `(i32, i32, i32, i32, ptr, len) -> i32` |
| `display_rect` | `(i32, i32, i32, i32, i32) -> i32` |
| `display_rect_outline` | `(i32, i32, i32, i32, i32) -> i32` |
| `display_rounded_rect` | `(i32, i32, i32, i32, i32, i32) -> i32` |
| `display_rounded_rect_fill` | `(i32, i32, i32, i32, i32, i32) -> i32` |
| `display_text` | `(i32, i32, string, i32) -> i32` |
| `display_text_large` | `(i32, i32, string, i32) -> i32` |
| `display_triangle` | `(i32, i32, i32, i32, i32, i32, i32) -> i32` |
| `display_triangle_fill` | `(i32, i32, i32, i32, i32, i32, i32) -> i32` |
| `display_vline` | `(i32, i32, i32, i32) -> i32` |

### GPIO

Gated by `CONFIG_GPIO`. Capability: `gpio.read` / `gpio.write`.

| Symbol | Signature |
|--------|-----------|
| `gpio_configure` | `(i32, i32) -> i32` |
| `gpio_read` | `(i32) -> i32` |
| `gpio_write` | `(i32, i32) -> i32` |

### RF / Radio

Gated by `CONFIG_AKIRA_MODULE_RF`. Capability: `rf.transceive`.

| Symbol | Signature |
|--------|-----------|
| `rf_get_rssi` | `() -> i32` |
| `rf_raw_capture` | `(i32, i32, i32, i32) -> i32` |
| `rf_raw_replay` | `(i32, i32, i32, i32) -> i32` |
| `rf_receive` | `(i32, i32, i32) -> i32` |
| `rf_recv_pop` | `(i32, i32, i32) -> i32` |
| `rf_select` | `(i32) -> i32` |
| `rf_send` | `(i32, i32) -> i32` |
| `rf_set_bandwidth` | `(i32) -> i32` |
| `rf_set_coding_rate` | `(i32) -> i32` |
| `rf_set_frequency` | `(i32) -> i32` |
| `rf_set_modulation` | `(i32) -> i32` |
| `rf_set_power` | `(i32) -> i32` |
| `rf_set_spreading_factor` | `(i32) -> i32` |

### WiFi (raw 802.11)

Gated by `CONFIG_WIFI`. Capability: `wifi.inject`.

| Symbol | Signature |
|--------|-----------|
| `wifi_deauth` | `(ptr, ptr, i32, i32, i32) -> i32` |
| `wifi_scan_aps` | `(ptr, len) -> i32` |
| `wifi_scan_rssi` | `(i32, i32) -> i32` |

### Sensors

Gated by `CONFIG_SENSOR`. Capability: `sensor.read`.

| Symbol | Signature |
|--------|-----------|
| `sensor_read` | `(i32) -> i32` |

### Memory

Gated by `CONFIG_AKIRA_WASM_MEMORY`. Capability: `memory`.

| Symbol | Signature |
|--------|-----------|
| `mem_alloc` | `(i32) -> i32` |
| `mem_free` | `(i32) -> void` |

### BLE (app GATT service)

Gated by `CONFIG_AKIRA_WASM_BLE`. Capability: `ble`.

| Symbol | Signature |
|--------|-----------|
| `ble_add_service` | `(i32) -> i32` |
| `ble_advertise` | `() -> i32` |
| `ble_char_create` | `(string, i32, i32) -> i32` |
| `ble_char_read` | `(i32, i32, i32) -> i32` |
| `ble_char_write` | `(i32, i32, i32) -> i32` |
| `ble_deinit` | `() -> i32` |
| `ble_event_pop` | `(i32, i32) -> i32` |
| `ble_init` | `() -> i32` |
| `ble_is_connected` | `() -> i32` |
| `ble_service_add_char` | `(i32, i32) -> i32` |
| `ble_service_create` | `(string) -> i32` |
| `ble_set_advertised_service` | `(i32) -> i32` |
| `ble_set_local_name` | `(string) -> i32` |
| `ble_stop_advertise` | `() -> i32` |

### HID

Gated by `CONFIG_AKIRA_WASM_HID`. Capability: `hid`.

| Symbol | Signature |
|--------|-----------|
| `hid_action_register` | `(string, i32, i32) -> i32` |
| `hid_action_trigger` | `(string) -> i32` |
| `hid_consumer_send` | `(i32) -> i32` |
| `hid_disable` | `() -> i32` |
| `hid_enable` | `() -> i32` |
| `hid_fido_recv` | `(i32, i32) -> i32` |
| `hid_fido_send` | `(i32, i32) -> i32` |
| `hid_gamepad_press` | `(i32) -> i32` |
| `hid_gamepad_release` | `(i32) -> i32` |
| `hid_gamepad_reset` | `() -> i32` |
| `hid_gamepad_set_axis` | `(i32, i32) -> i32` |
| `hid_gamepad_set_dpad` | `(i32) -> i32` |
| `hid_init` | `(i32, i32) -> i32` |
| `hid_is_connected` | `() -> i32` |
| `hid_key_press` | `(i32) -> i32` |
| `hid_key_release` | `(i32) -> i32` |
| `hid_key_release_all` | `() -> i32` |
| `hid_mouse_btn_press` | `(i32) -> i32` |
| `hid_mouse_btn_release` | `(i32) -> i32` |
| `hid_mouse_move` | `(i32, i32) -> i32` |
| `hid_mouse_scroll` | `(i32) -> i32` |
| `hid_raw_recv` | `(i32, i32) -> i32` |
| `hid_send_raw_report` | `(i32, i32, i32) -> i32` |
| `hid_set_device_types` | `(i32) -> i32` |
| `hid_set_modifiers` | `(i32) -> i32` |
| `hid_set_transport` | `(i32) -> i32` |
| `hid_type_string` | `(string) -> i32` |

### App lifecycle

Gated by `CONFIG_AKIRA_WASM_LIFECYCLE`. Capability: `app.control` / `app.info` / `app.switch`.

| Symbol | Signature |
|--------|-----------|
| `app_check_update` | `(i32, i32) -> i32` |
| `app_get_self_name` | `(i32, i32) -> i32` |
| `app_get_status` | `(string) -> i32` |
| `app_list` | `(i32, i32) -> i32` |
| `app_request_update` | `() -> i32` |
| `app_start` | `(string) -> i32` |
| `app_stop` | `(string) -> i32` |
| `app_switch` | `(string) -> i32` |

### IPC (message bus)

Gated by `CONFIG_AKIRA_WASM_IPC`. Capability: `ipc`.

| Symbol | Signature |
|--------|-----------|
| `msg_pending` | `(string) -> i32` |
| `msg_publish` | `(string, i32, i32) -> i32` |
| `msg_recv` | `(string, i32, i32, i32) -> i32` |
| `msg_subscribe` | `(string) -> i32` |
| `msg_try_recv` | `(string, i32, i32) -> i32` |
| `msg_unsubscribe` | `(string) -> i32` |

### Timers

Gated by `CONFIG_AKIRA_WASM_TIMER`. Capability: `timer`.

| Symbol | Signature |
|--------|-----------|
| `timer_create` | `() -> i32` |
| `timer_elapsed` | `(i32) -> i32` |
| `timer_free` | `(i32) -> i32` |
| `timer_start` | `(i32) -> i32` |
| `timer_stop` | `(i32) -> i32` |

### UART

Gated by `CONFIG_AKIRA_WASM_UART`. Capability: `uart`.

| Symbol | Signature |
|--------|-----------|
| `uart_close` | `(i32) -> i32` |
| `uart_open` | `(i32, i32) -> i32` |
| `uart_read` | `(i32, ptr, len) -> i32` |
| `uart_write` | `(i32, ptr, len) -> i32` |

### I2C

Gated by `CONFIG_AKIRA_WASM_I2C`. Capability: `i2c`.

| Symbol | Signature |
|--------|-----------|
| `i2c_read_reg` | `(i32, i32, i32, ptr, len) -> i32` |
| `i2c_write_reg` | `(i32, i32, i32, ptr, len) -> i32` |

### PWM

Gated by `CONFIG_AKIRA_WASM_PWM`. Capability: `pwm`.

| Symbol | Signature |
|--------|-----------|
| `pwm_disable` | `(i32) -> i32` |
| `pwm_set` | `(i32, i32, i32) -> i32` |

### ADC

Gated by `CONFIG_AKIRA_WASM_ADC`. Capability: `adc`.

| Symbol | Signature |
|--------|-----------|
| `adc_read` | `(i32) -> i32` |
| `adc_read_mv` | `(i32) -> i32` |

### Watchdog

Gated by `CONFIG_AKIRA_WASM_WDT`. Capability: `wdt`.

| Symbol | Signature |
|--------|-----------|
| `wdt_pet` | `() -> i32` |

### Storage (app sandbox)

Gated by `CONFIG_AKIRA_WASM_STORAGE`. Capability: `storage.read` / `storage.write`.

| Symbol | Signature |
|--------|-----------|
| `storage_close` | `(i32) -> void` |
| `storage_delete` | `(string) -> i32` |
| `storage_list` | `(string, ptr, len) -> i32` |
| `storage_open` | `(string, i32) -> i32` |
| `storage_read` | `(i32, ptr, len) -> i32` |
| `storage_write` | `(i32, ptr, len) -> i32` |

### Network sockets

Gated by `CONFIG_AKIRA_WASM_NET`. Capability: `network.*`.

| Symbol | Signature |
|--------|-----------|
| `net_bind` | `(i32, i32) -> i32` |
| `net_close` | `(i32) -> i32` |
| `net_connect` | `(i32, string, i32) -> i32` |
| `net_event_pop` | `(i32, i32) -> i32` |
| `net_get_ip` | `(i32, i32) -> i32` |
| `net_listen` | `(i32, i32) -> i32` |
| `net_open` | `(i32) -> i32` |
| `net_rx_bind` | `(i32, i32, i32) -> i32` |
| `net_tx_bind` | `(i32, i32, i32) -> i32` |
| `net_tx_flush` | `(i32) -> i32` |

### System / SD

Gated by `CONFIG_AKIRA_SYSTEM_API`. Capability: `app.control`.

| Symbol | Signature |
|--------|-----------|
| `app_install_from_sd` | `(string) -> i32` |
| `sd_scan_wasm` | `(ptr, len) -> i32` |

### Power

Gated by `CONFIG_AKIRA_WASM_POWER`. Capability: `power.read` / `power.control`.

| Symbol | Signature |
|--------|-----------|
| `power_get_battery_level` | `() -> i32` |
| `power_get_battery_status` | `(ptr, len) -> i32` |
| `power_get_mode` | `() -> i32` |
| `power_set_low_power` | `(i32) -> i32` |
| `power_set_mode` | `(i32) -> i32` |
| `power_wake_on_gpio` | `(i32, i32) -> i32` |
| `power_wake_on_timer` | `(i32) -> i32` |

### Settings

Gated by `CONFIG_AKIRA_WASM_SETTINGS`. Capability: `settings.*`.

| Symbol | Signature |
|--------|-----------|
| `settings_delete` | `(string) -> i32` |
| `settings_get` | `(string, i32, i32) -> i32` |
| `settings_set` | `(string, string) -> i32` |

### Filesystem

Gated by `CONFIG_AKIRA_WASM_FS`. Capability: `fs.read` / `fs.write`.

| Symbol | Signature |
|--------|-----------|
| `fs_close` | `(i32) -> i32` |
| `fs_mkdir` | `(string) -> i32` |
| `fs_open` | `(string, i32) -> i32` |
| `fs_read` | `(i32, ptr, len) -> i32` |
| `fs_readdir` | `(string, ptr, len) -> i32` |
| `fs_seek` | `(i32, i32, i32) -> i32` |
| `fs_stat` | `(string, ptr) -> i32` |
| `fs_tell` | `(i32) -> i32` |
| `fs_unlink` | `(string) -> i32` |
| `fs_write` | `(i32, ptr, len) -> i32` |

### Crypto

Gated by `CONFIG_AKIRA_WASM_CRYPTO`. Capability: `crypto`.

| Symbol | Signature |
|--------|-----------|
| `crypto_aes256_ctr` | `(ptr, ptr, i32, ptr, len, ptr) -> i32` |
| `crypto_aes256_decrypt` | `(ptr, ptr, i32, ptr, len, ptr) -> i32` |
| `crypto_aes256_encrypt` | `(ptr, ptr, i32, ptr, len, ptr) -> i32` |
| `crypto_ed25519_keygen` | `(ptr, ptr) -> i32` |
| `crypto_ed25519_sign` | `(ptr, ptr, len, ptr) -> i32` |
| `crypto_hmac_sha256` | `(ptr, len, ptr, len, ptr) -> i32` |
| `crypto_random` | `(ptr, len) -> i32` |
| `crypto_sha256` | `(ptr, len, ptr) -> i32` |

### RTC

Gated by `CONFIG_AKIRA_WASM_RTC`. Capability: `rtc.read` / `rtc.write`.

| Symbol | Signature |
|--------|-----------|
| `rtc_alarm_fired` | `() -> i32` |
| `rtc_get_unix_time` | `() -> i32` |
| `rtc_get_uptime_ms` | `() -> i32` |
| `rtc_set_alarm` | `(i32) -> i32` |
| `rtc_set_unix_time` | `(i32) -> i32` |

### OTA

Gated by `CONFIG_AKIRA_WASM_OTA`. Capability: `ota.trigger`.

| Symbol | Signature |
|--------|-----------|
| `ota_check` | `(string) -> i32` |
| `ota_confirm` | `() -> i32` |
| `ota_fetch_and_apply` | `(string) -> i32` |
| `ota_get_state` | `() -> i32` |
| `ota_rollback` | `() -> i32` |

### Edge AI inference

Gated by `CONFIG_AKIRA_WASM_AIINFER`. Capability: `ai.infer`.

| Symbol | Signature |
|--------|-----------|
| `aiinfer_load` | `(ptr, len) -> i32` |
| `aiinfer_run` | `(i32, ptr, len, ptr, len) -> i32` |
| `aiinfer_unload` | `(i32) -> void` |

### Matter

Gated by `CONFIG_AKIRA_WASM_MATTER`. Capability: `matter`.

| Symbol | Signature |
|--------|-----------|
| `matter_cmd_poll` | `(ptr, ptr, ptr, ptr, len, i32) -> i32` |
| `matter_commission` | `(string, ptr, len) -> i32` |
| `matter_endpoint_add` | `(i32, ptr, len) -> i32` |
| `matter_get_pairing` | `(ptr, len, ptr, len) -> i32` |
| `matter_open_pairing` | `(i32) -> i32` |
| `matter_poll` | `(ptr, len, ptr, i32, ptr, len, i32, i32) -> i32` |
| `matter_report_attr` | `(i32, i32, i32, ptr, len) -> i32` |
| `matter_send` | `(ptr, len, ptr, len, i32) -> i32` |
| `matter_subscribe` | `(ptr, len, i32) -> i32` |

### MQTT / Home Assistant

Gated by `CONFIG_AKIRA_WASM_MQTT`. Capability: `mqtt`.

| Symbol | Signature |
|--------|-----------|
| `ha_light_poll` | `(string, ptr, ptr, ptr, ptr, ptr, i32) -> i32` |
| `ha_light_register` | `(string, string) -> i32` |
| `ha_light_report` | `(string, i32, i32, i32, i32, i32) -> i32` |
| `mqtt_connected` | `() -> i32` |
| `mqtt_poll` | `(ptr, len, ptr, len, i32) -> i32` |
| `mqtt_publish` | `(string, ptr, len, i32, i32) -> i32` |
| `mqtt_subscribe` | `(string) -> i32` |

### AkiraMesh

Gated by `CONFIG_AKIRA_WASM_MESH`. Capability: `mesh`.

| Symbol | Signature |
|--------|-----------|
| `mesh_broadcast` | `(ptr, len, i32) -> i32` |
| `mesh_distribute_app` | `(ptr, string, ptr, len) -> i32` |
| `mesh_get_nodes` | `(ptr, i32) -> i32` |
| `mesh_get_stats` | `(ptr) -> i32` |
| `mesh_init` | `(i32, string, i32, i32) -> i32` |
| `mesh_recv_pop` | `(ptr, ptr, len, i32) -> i32` |
| `mesh_send` | `(ptr, ptr, len) -> i32` |
| `mesh_start` | `() -> i32` |
| `mesh_stop` | `() -> i32` |

### Not available

These are declared in headers or gated by Kconfig but register **no** native symbol:

| Name | Status |
|------|--------|
| `input_*` | No button input API exists. Nothing is registered and nothing is declared in `include/akira_native_api.h`. Use `CONFIG_AKIRA_LVGL_INPUT_ZEPHYR` instead. |
| `sync_*` | AkiraSync is not compiled — `src/connectivity/sync/` is not referenced by `CMakeLists.txt`. `AKIRA_CAP_SYNC` is reserved. |
| `crypto_ed25519_verify` | Does not exist. Only `crypto_ed25519_keygen()` and `crypto_ed25519_sign()` are exported. |

---

## Related Documentation

- [SDK API Reference](../development/sdk-api-reference.md) - High-level SDK functions with examples
- [Best Practices](../development/best-practices.md) - Write efficient apps
- [API Overview](index.md) - Quick reference
- [Manifest Format](manifest-format.md) - Capability declarations
- [Security Model](../architecture/security.md) - Permission system
- [Building Apps](../development/building-apps.md) - WASM compilation

---

*Last updated: 2026-09-14 (AkiraOS v1.6.5)*
