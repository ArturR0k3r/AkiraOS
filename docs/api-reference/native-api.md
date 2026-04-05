# Native API Reference

Complete reference for all AkiraOS native functions callable from WASM.

> **Note:** This is a custom API, **not WASI**. It's designed specifically for embedded systems and real-time constraints.
> 
> For the full, up-to-date function list with examples, see the canonical **[AkiraSDK API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.4.x/docs/API_REFERENCE.md)**.  
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

> Full display API (20+ functions: `display_line`, `display_circle`, `display_bitmap`, `display_progress_bar`, `display_rounded_rect`, etc.) — see [AkiraSDK API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.4.x/docs/API_REFERENCE.md#display-api).

---

## Input Functions

> `input_read_buttons`, `input_read_touch`, and `input_read_analog` are not registered native exports in the current runtime (`src/api/akira_export_api.c`). Check [AkiraSDK API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.4.x/docs/API_REFERENCE.md) for the current input API.

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
> See [AkiraSDK API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.4.x/docs/API_REFERENCE.md#rf-api) for full RF reference.

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

## Related Documentation

- [SDK API Reference](../development/sdk-api-reference.md) - High-level SDK functions with examples
- [Best Practices](../development/best-practices.md) - Write efficient apps
- [API Overview](index.md) - Quick reference
- [Manifest Format](manifest-format.md) - Capability declarations
- [Security Model](../architecture/security.md) - Permission system
- [Building Apps](../development/building-apps.md) - WASM compilation
