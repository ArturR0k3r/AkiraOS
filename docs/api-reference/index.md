# API Reference Overview

Complete reference for AkiraOS WASM application APIs.

> **Looking for the high-level SDK API?** See the [SDK API Reference](../development/sdk-api-reference.md) for event system, display, input, sensors, RF, storage, network, and system functions with full examples.

## Available APIs

AkiraOS provides a custom native API for WASM applications. This is **not** WASI—it's optimized for embedded systems and real-time constraints.

### API Categories

| Category | Functions | Purpose |
|----------|-----------|---------|
| [Display](#display-api) | 5 functions | Screen rendering |
| [Input](#input-api) | 3 functions | Button/touch input |
| [Sensors](#sensor-api) | 2 functions | IMU, temperature, etc. |
| [RF/Network](#rf-api) | 4 functions | WiFi, BT, LoRa |
| [File System](#filesystem-api) | 5 functions | Persistent storage |
| [Logging](#logging-api) | 3 functions | Debug output |
| [Time](#time-api) | 2 functions | Timing and delays |

## Import Module

All native functions are imported from the `"akira"` module:

```c
__attribute__((import_module("akira")))
__attribute__((import_name("log")))
extern void akira_log(const char *message, uint32_t len);
```

## Display API

### `display_clear`
Clear the entire screen to a specific color.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("display_clear")))
extern int akira_display_clear(uint32_t color);
```

**Parameters:**
- `color`: 24-bit RGB color (0xRRGGBB)

**Returns:** 0 on success, negative on error

**Capability Required:** `CAP_DISPLAY_WRITE`

---

### `display_pixel`
Draw a single pixel.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("display_pixel")))
extern int akira_display_pixel(uint32_t x, uint32_t y, uint32_t color);
```

**Parameters:**
- `x`, `y`: Pixel coordinates
- `color`: 24-bit RGB color

**Returns:** 0 on success, negative on error

**Capability Required:** `CAP_DISPLAY_WRITE`

---

### `display_rect`
Draw a filled rectangle.

```c
extern int akira_display_rect(uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height,
                              uint32_t color);
```

---

### `display_text`
Render text string.

```c
extern int akira_display_text(uint32_t x, uint32_t y,
                              const char *text, uint32_t len,
                              uint32_t color);
```

---

### `display_flush`
Flush framebuffer to screen (if buffered).

```c
extern int akira_display_flush();
```

---

## Input API

### `input_read_buttons`
Read digital button states.

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

---

## Sensor API

### `sensor_read`
Read sensor value by ID.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("sensor_read")))
extern int akira_sensor_read(uint32_t sensor_id, float *data);
```

**Parameters:**
- `sensor_id`: Sensor identifier (0-15)
- `data`: Pointer to receive sensor value

**Returns:** 0 on success, negative on error

**Capability Required:** `CAP_SENSOR_READ`

**Sensor IDs:**
- 0: Temperature (°C)
- 1: Humidity (%)
- 2: Pressure (hPa)
- 3: Accel X (m/s²)
- 4: Accel Y
- 5: Accel Z
- 6: Gyro X (°/s)
- 7: Gyro Y
- 8: Gyro Z

---

## RF/Network API

### `rf_send`
Send data via RF interface.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("rf_send")))
extern int akira_rf_send(const uint8_t *data, uint32_t len);
```

**Capability Required:** `CAP_RF_TRANSCEIVE`

**Max Length:** 256 bytes per call

---

### `rf_recv`
Receive data from RF interface.

```c
extern int akira_rf_recv(uint8_t *buffer, uint32_t max_len);
```

**Returns:** Bytes received, or 0 if no data

---

## File System API

### `fs_read`
Read file contents.

```c
extern int akira_fs_read(const char *path, uint8_t *buffer, uint32_t max_len);
```

**Capability Required:** `CAP_FS_READ`

**Path Restriction:** Apps can only access `/data/<app_name>/`

---

### `fs_write`
Write file contents.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("fs_write")))
extern int akira_fs_write(const char *path, const uint8_t *data, uint32_t len);
```

**Capability Required:** `CAP_FS_WRITE`

---

### `fs_stat`
Get file information.

```c
extern int akira_fs_stat(const char *path, uint32_t *size);
```

**Returns:** 0 if file exists, -1 otherwise

---

## Logging API

### `log`
Write log message.

```c
__attribute__((import_module("akira")))
__attribute__((import_name("log")))
extern void akira_log(const char *message, uint32_t len);
```

**No capability required** - Always available

**Max Length:** 256 characters

---

## Time API

### `time_ms`
Get current time in milliseconds since boot.

```c
extern uint64_t akira_time_ms();
```

---

### `sleep_ms`
Sleep for specified milliseconds.

```c
extern void akira_sleep_ms(uint32_t ms);
```

---

## Error Codes

| Code | Constant | Meaning |
|------|----------|---------|
| 0 | OK | Success |
| -1 | EPERM | Permission denied (capability) |
| -2 | ENOENT | File not found |
| -3 | EIO | I/O error |
| -12 | ENOMEM | Out of memory |
| -22 | EINVAL | Invalid argument |

---

## Complete Example

```c
#include <stdint.h>
#include <string.h>

// Import native functions
__attribute__((import_module("akira")))
__attribute__((import_name("log")))
extern void akira_log(const char *msg, uint32_t len);

__attribute__((import_module("akira")))
__attribute__((import_name("display_clear")))
extern int akira_display_clear(uint32_t color);

__attribute__((import_module("akira")))
__attribute__((import_name("input_read_buttons")))
extern uint32_t akira_input_read_buttons();

__attribute__((import_module("akira")))
__attribute__((import_name("sensor_read")))
extern int akira_sensor_read(uint32_t id, float *data);

// Entry point
__attribute__((export_name("_start")))
void app_main() {
    akira_log("App starting", 12);
    
    // Clear screen to blue
    akira_display_clear(0x0000FF);
    
    // Read temperature
    float temp = 0.0f;
    if (akira_sensor_read(0, &temp) == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Temp: %.2f C", temp);
        akira_log(msg, strlen(msg));
    }
    
    // Wait for button press
    while (!(akira_input_read_buttons() & 0x01)) {
        // Busy wait (bad practice, use sleep in real app)
    }
    
    akira_log("Button pressed!", 14);
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
