# Native API Reference

Complete reference for all AkiraOS native functions callable from WASM.

> **Note:** This is a custom API, **not WASI**. It's designed specifically for embedded systems and real-time constraints.

## Import Declaration

All functions must be imported from the `"akira"` module:

```c
__attribute__((import_module("akira")))
__attribute__((import_name("function_name")))
extern return_type akira_function_name(parameters);
```

---

## Display Functions

### `display_clear(color)`

Clear the entire display to a solid color.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("display_clear")))
extern int akira_display_clear(uint32_t color);
```

**Parameters:**
- `color` (uint32_t): 24-bit RGB color in format 0xRRGGBB

**Returns:** 
- `0`: Success
- `-EACCES`: Permission denied (missing `CAP_DISPLAY_WRITE`)
- `-EIO`: Hardware error

**Capability Required:** `CAP_DISPLAY_WRITE`

**Example:**
```c
akira_display_clear(0x000000);  // Black
akira_display_clear(0xFF0000);  // Red
akira_display_clear(0x00FF00);  // Green
akira_display_clear(0x0000FF);  // Blue
```

---

### `display_pixel(x, y, color)`

Draw a single pixel at specified coordinates.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("display_pixel")))
extern int akira_display_pixel(uint32_t x, uint32_t y, uint32_t color);
```

**Parameters:**
- `x` (uint32_t): X coordinate (0 = left edge)
- `y` (uint32_t): Y coordinate (0 = top edge)
- `color` (uint32_t): 24-bit RGB color

**Returns:** Same as `display_clear`

**Capability Required:** `CAP_DISPLAY_WRITE`

**Example:**
```c
// Draw a red pixel at (100, 50)
akira_display_pixel(100, 50, 0xFF0000);
```

---

### `display_rect(x, y, width, height, color)`

Draw a filled rectangle.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("display_rect")))
extern int akira_display_rect(uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height,
                              uint32_t color);
```

**Capability Required:** `CAP_DISPLAY_WRITE`

---

### `display_text(x, y, text, len, color)`

Render text string at specified position.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("display_text")))
extern int akira_display_text(uint32_t x, uint32_t y,
                              const char *text, uint32_t len,
                              uint32_t color);
```

**Parameters:**
- `text`: Pointer to string (WASM memory)
- `len`: String length (bytes)

**Capability Required:** `CAP_DISPLAY_WRITE`

---

### `display_flush()`

Flush framebuffer to physical display (if buffered rendering enabled).

```c
__attribute__((import_module("akira")))
__attribute__((import_name("display_flush")))
extern int akira_display_flush();
```

**Note:** Only needed if `CONFIG_DISPLAY_BUFFERED=y`

---

## Input Functions

### `input_read_buttons()`

Read current state of all digital buttons.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("input_read_buttons")))
extern uint32_t akira_input_read_buttons();
```

**Returns:** Bitmask where each bit represents a button (1 = pressed, 0 = not pressed)

**Capability Required:** `CAP_INPUT_READ`

**Button Mapping:**
- Bit 0: Button A (primary action)
- Bit 1: Button B (secondary action)
- Bit 2: Button C
- Bit 3: Button D
- Bits 4-31: Reserved

**Example:**
```c
uint32_t buttons = akira_input_read_buttons();

if (buttons & 0x01) {
    akira_log("Button A pressed", 17);
}

if (buttons & 0x02) {
    akira_log("Button B pressed", 17);
}

// Check multiple buttons
if ((buttons & 0x03) == 0x03) {
    akira_log("A and B pressed together", 24);
}
```

---

### `input_read_touch(x_out, y_out)`

Read touchscreen coordinates (if available).

```c
__attribute__((import_module("akira")))
__attribute__((import_name("input_read_touch")))
extern int akira_input_read_touch(uint32_t *x_out, uint32_t *y_out);
```

**Parameters:**
- `x_out`: Pointer to receive X coordinate
- `y_out`: Pointer to receive Y coordinate

**Returns:**
- `0`: Touch detected, coordinates written
- `-1`: No touch detected

**Capability Required:** `CAP_INPUT_READ`

---

### `input_read_analog(channel)`

Read analog input (ADC) value.

```c
extern uint32_t akira_input_read_analog(uint32_t channel);
```

**Parameters:**
- `channel`: ADC channel (0-7)

**Returns:** Raw ADC value (0-4095 for 12-bit ADC)

**Capability Required:** `CAP_INPUT_READ`

---

## Sensor Functions

### `sensor_read(sensor_id, data_out)`

Read sensor value by ID.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("sensor_read")))
extern int akira_sensor_read(uint32_t sensor_id, float *data_out);
```

**Parameters:**
- `sensor_id`: Sensor identifier (see table below)
- `data_out`: Pointer to receive sensor value

**Returns:**
- `0`: Success, value written to `data_out`
- `-ENOENT`: Sensor not available
- `-EACCES`: Permission denied

**Capability Required:** `CAP_SENSOR_READ`

**Sensor IDs:**
| ID | Sensor | Unit | Range |
|----|--------|------|-------|
| 0 | Temperature | °C | -40 to 85 |
| 1 | Humidity | % | 0 to 100 |
| 2 | Pressure | hPa | 300 to 1100 |
| 3 | Accelerometer X | m/s² | -156 to 156 |
| 4 | Accelerometer Y | m/s² | -156 to 156 |
| 5 | Accelerometer Z | m/s² | -156 to 156 |
| 6 | Gyroscope X | °/s | -2000 to 2000 |
| 7 | Gyroscope Y | °/s | -2000 to 2000 |
| 8 | Gyroscope Z | °/s | -2000 to 2000 |
| 9 | Magnetometer X | μT | -4900 to 4900 |
| 10 | Magnetometer Y | μT | -4900 to 4900 |
| 11 | Magnetometer Z | μT | -4900 to 4900 |

**Example:**
```c
float temp = 0.0f;
int ret = akira_sensor_read(0, &temp);

if (ret == 0) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Temperature: %.2f C", temp);
    akira_log(msg, strlen(msg));
} else {
    akira_log("Sensor read failed", 18);
}
```

---

### `sensor_list(buffer, max_count)`

Get list of available sensors.

```c
extern int akira_sensor_list(uint32_t *buffer, uint32_t max_count);
```

**Returns:** Number of sensors written to buffer

---

## RF/Network Functions

### `rf_send(data, length)`

Send data via active RF interface (WiFi, Bluetooth, or LoRa).

```c
__attribute__((import_module("akira")))
__attribute__((import_name("rf_send")))
extern int akira_rf_send(const uint8_t *data, uint32_t length);
```

**Parameters:**
- `data`: Pointer to data buffer (WASM memory)
- `length`: Number of bytes to send (max 256)

**Returns:**
- `>= 0`: Bytes sent
- `-EACCES`: Permission denied
- `-EINVAL`: Invalid length

**Capability Required:** `CAP_RF_TRANSCEIVE`

**Example:**
```c
const char *message = "Hello, RF!";
int sent = akira_rf_send((uint8_t*)message, strlen(message));

if (sent < 0) {
    akira_log("Send failed", 11);
}
```

---

### `rf_recv(buffer, max_length)`

Receive data from RF interface (non-blocking).

```c
__attribute__((import_module("akira")))
__attribute__((import_name("rf_recv")))
extern int akira_rf_recv(uint8_t *buffer, uint32_t max_length);
```

**Returns:**
- `> 0`: Bytes received
- `0`: No data available
- `< 0`: Error

**Capability Required:** `CAP_RF_TRANSCEIVE`

---

### `rf_status()`

Get RF interface status.

```c
extern uint32_t akira_rf_status();
```

**Returns:** Status bitmask
- Bit 0: Connected
- Bit 1: Transmitting
- Bit 2: Receiving

---

## File System Functions

### `fs_read(path, buffer, max_length)`

Read file contents into buffer.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("fs_read")))
extern int akira_fs_read(const char *path, uint8_t *buffer, uint32_t max_length);
```

**Parameters:**
- `path`: File path (must start with `/data/<app_name>/`)
- `buffer`: Destination buffer
- `max_length`: Maximum bytes to read

**Returns:** Bytes read, or negative on error

**Capability Required:** `CAP_FS_READ`

**Path Restrictions:** Apps can only access `/data/<app_name>/` directory

---

### `fs_write(path, data, length)`

Write data to file.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("fs_write")))
extern int akira_fs_write(const char *path, const uint8_t *data, uint32_t length);
```

**Capability Required:** `CAP_FS_WRITE`

---

### `fs_stat(path, size_out)`

Get file information.

```c
extern int akira_fs_stat(const char *path, uint32_t *size_out);
```

**Returns:**
- `0`: File exists, size written
- `-ENOENT`: File not found

---

### `fs_delete(path)`

Delete file.

```c
extern int akira_fs_delete(const char *path);
```

**Capability Required:** `CAP_FS_WRITE`

---

### `fs_list(path, buffer, max_entries)`

List directory contents.

```c
extern int akira_fs_list(const char *path, char *buffer, uint32_t max_entries);
```

---

## Logging Functions

### `log(message, length)`

Write log message to system log.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("log")))
extern void akira_log(const char *message, uint32_t length);
```

**Parameters:**
- `message`: Log string (max 256 chars)
- `length`: String length

**No capability required** - Always available for debugging

**Example:**
```c
const char *msg = "Processing started";
akira_log(msg, 18);
```

---

### `log_debug(message)`

Log at DEBUG level.

```c
extern void akira_log_debug(const char *message, uint32_t length);
```

---

### `log_error(message)`

Log at ERROR level.

```c
extern void akira_log_error(const char *message, uint32_t length);
```

---

## Time Functions

### `time_ms()`

Get current time in milliseconds since boot.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("time_ms")))
extern uint64_t akira_time_ms();
```

**Returns:** Milliseconds (wraps after ~49 days)

**Example:**
```c
uint64_t start = akira_time_ms();
// ... do work ...
uint64_t elapsed = akira_time_ms() - start;
```

---

### `sleep_ms(milliseconds)`

Sleep for specified duration (yields to other apps).

```c
__attribute__((import_module("akira")))
__attribute__((import_name("sleep_ms")))
extern void akira_sleep_ms(uint32_t milliseconds);
```

**Recommended** instead of busy-wait loops for power efficiency.

---

## Error Codes

Standard POSIX error codes (negative values):

| Code | Name | Description |
|------|------|-------------|
| -1 | `EPERM` | Operation not permitted |
| -2 | `ENOENT` | No such file or directory |
| -3 | `EIO` | I/O error |
| -12 | `ENOMEM` | Out of memory |
| -13 | `EACCES` | Permission denied (capability) |
| -22 | `EINVAL` | Invalid argument |
| -28 | `ENOSPC` | No space left on device |

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

## Related Documentation

- [API Overview](index.md) - Quick reference
- [Manifest Format](manifest-format.md) - Capability declarations
- [Security Model](../architecture/security.md) - Permission system
- [Building Apps](../development/building-apps.md) - WASM compilation
