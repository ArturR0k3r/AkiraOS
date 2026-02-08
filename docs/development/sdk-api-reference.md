# 📚 Akira SDK API Reference

Complete reference for all Akira SDK APIs with detailed descriptions, parameters, and return values.

---

## 📑 Table of Contents

- [Event System](#-event-system)
- [Display API](#-display-api)
- [Input API](#-input-api)
- [RF API](#-rf-api)
- [GPIO & Timer API](#-gpio--timer-api)
- [Sensor API](#-sensor-api)
- [Storage API](#-storage-api)
- [Network API](#-network-api)
- [System API](#-system-api)
- [Constants & Types](#-constants--types)

---

## 🎯 Event System

The event system is the **heart** of Akira SDK. All hardware events (timers, GPIO, messages) flow through this system.

### `akira_process_events()`

**Process pending events from the runtime.**

```c
void akira_process_events(void);
```

**Description:**
Call this function repeatedly in your main loop. It processes up to 5 events per call to prevent blocking. If no events are available, it sleeps for `IDLE_SLEEP_MS` to avoid busy-looping.

**Example:**
```c
while(1) {
    akira_process_events();  // Handle all pending events
}
```

**Best Practices:**
- ✅ Call in every iteration of your main loop
- ✅ Keep callbacks short and non-blocking
- ❌ Don't call from within a callback
- ❌ Don't use infinite loops in callbacks

---

### Event Types

```c
typedef enum {
    AKIRA_EVENT_TYPE_TIMER,    // Timer fired
    AKIRA_EVENT_TYPE_GPIO,     // GPIO state changed
    AKIRA_EVENT_TYPE_MESSAGE   // Message received
} akira_event_type_t;
```

---

## 📺 Display API

**Capability Required:** `display.write`

Create user interfaces with RGB565 graphics. The display uses a framebuffer that must be flushed to become visible.

### `akira_display_clear()`

**Clear the entire display with a solid color.**

```c
void akira_display_clear(uint16_t color);
```

**Parameters:**
- `color` - RGB565 color value (e.g., `0xFFFF` for white, `0x0000` for black)

**Example:**
```c
akira_display_clear(0x001F);  // Blue background
```

---

### `akira_display_pixel()`

**Draw a single pixel.**

```c
void akira_display_pixel(int x, int y, uint16_t color);
```

**Parameters:**
- `x` - X coordinate
- `y` - Y coordinate
- `color` - RGB565 color value

**Example:**
```c
akira_display_pixel(50, 50, 0xF800);  // Red pixel at (50, 50)
```

---

### `akira_display_rect()`

**Draw a filled rectangle.**

```c
void akira_display_rect(int x, int y, int w, int h, uint16_t color);
```

**Parameters:**
- `x` - X coordinate (top-left corner)
- `y` - Y coordinate (top-left corner)
- `w` - Width in pixels
- `h` - Height in pixels
- `color` - RGB565 color value

**Example:**
```c
akira_display_rect(10, 10, 100, 50, 0x07E0);  // Green 100x50 rectangle
```

---

### `akira_display_text()`

**Draw text string.**

```c
void akira_display_text(int x, int y, const char *text, uint16_t color);
```

**Parameters:**
- `x` - X coordinate (baseline start)
- `y` - Y coordinate (baseline)
- `text` - Null-terminated string
- `color` - RGB565 color value

**Example:**
```c
akira_display_text(10, 20, "Hello World!", 0xFFFF);
```

---

### `akira_display_flush()`

**Flush the framebuffer to the physical display.**

```c
void akira_display_flush(void);
```

**Description:**
All drawing operations are buffered. Call this function to make them visible on the screen.

**Example:**
```c
akira_display_clear(0x0000);
akira_display_text(10, 10, "Ready!", 0xFFFF);
akira_display_flush();  // Now visible!
```

---

### `akira_display_get_size()`

**Get display dimensions.**

```c
void akira_display_get_size(int *width, int *height);
```

**Parameters:**
- `width` - Output pointer for display width
- `height` - Output pointer for display height

**Example:**
```c
int w, h;
akira_display_get_size(&w, &h);
printf("Display is %dx%d\n", w, h);
```

---

### 🎨 RGB565 Color Format

RGB565 packs colors into 16 bits: **5 bits red, 6 bits green, 5 bits blue**

```c
// Common colors
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F

// Create custom color
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
```

---

## 🎮 Input API

**Capability Required:** `input.read`

Handle button input with polling or event-driven callbacks.

### Button Constants

```c
#define AKIRA_BTN_POWER    (1 << 0)   // Power button
#define AKIRA_BTN_SETTINGS (1 << 1)   // Settings button
#define AKIRA_BTN_UP       (1 << 2)   // D-pad up
#define AKIRA_BTN_DOWN     (1 << 3)   // D-pad down
#define AKIRA_BTN_LEFT     (1 << 4)   // D-pad left
#define AKIRA_BTN_RIGHT    (1 << 5)   // D-pad right
#define AKIRA_BTN_A        (1 << 6)   // A button
#define AKIRA_BTN_B        (1 << 7)   // B button
#define AKIRA_BTN_X        (1 << 8)   // X button
#define AKIRA_BTN_Y        (1 << 9)   // Y button
```

---

### `akira_input_read_buttons()`

**Read current button state.**

```c
uint32_t akira_input_read_buttons(void);
```

**Returns:** Bitmask of currently pressed buttons

**Example:**
```c
uint32_t buttons = akira_input_read_buttons();
if (buttons & AKIRA_BTN_A) {
    printf("A button is pressed!\n");
}
```

---

### `akira_input_button_pressed()`

**Check if a specific button is pressed.**

```c
bool akira_input_button_pressed(uint32_t button);
```

**Parameters:**
- `button` - Button mask (e.g., `AKIRA_BTN_A`)

**Returns:** `true` if the button is pressed, `false` otherwise

---

### `akira_input_set_callback()`

**Set callback for button events.**

```c
typedef void (*akira_input_callback_t)(uint32_t buttons);
void akira_input_set_callback(akira_input_callback_t callback);
```

**Parameters:**
- `callback` - Function to call when button state changes

**Example:**
```c
void on_button(uint32_t buttons) {
    if (buttons & AKIRA_BTN_A) {
        akira_log(2, "A pressed!");
    }
}

akira_input_set_callback(on_button);
```

---

## 📡 RF API

**Capability Required:** `rf.transceive`

Wireless communication with various RF chips.

### Supported Chips

```c
#define AKIRA_RF_CHIP_NONE     0
#define AKIRA_RF_CHIP_NRF24L01 1  // 2.4GHz
#define AKIRA_RF_CHIP_LR1121   2  // LoRa
#define AKIRA_RF_CHIP_CC1101   3  // Sub-GHz
#define AKIRA_RF_CHIP_SX1276   4  // LoRa
#define AKIRA_RF_CHIP_RFM69    5  // FSK/OOK
```

### RF Modes

```c
#define AKIRA_RF_MODE_IDLE  0
#define AKIRA_RF_MODE_RX    1  // Receive
#define AKIRA_RF_MODE_TX    2  // Transmit
#define AKIRA_RF_MODE_SLEEP 3  // Low power
```

---

### `akira_rf_init()`

**Initialize RF chip.**

```c
int akira_rf_init(akira_rf_chip_t chip);
```

**Parameters:**
- `chip` - RF chip type (e.g., `AKIRA_RF_CHIP_NRF24L01`)

**Returns:** `0` on success, negative error code on failure

---

### `akira_rf_send()`

**Send data packet.**

```c
int akira_rf_send(const uint8_t *data, size_t len);
```

**Parameters:**
- `data` - Data buffer to send
- `len` - Number of bytes to send

**Returns:** `0` on success, negative error code on failure

---

### `akira_rf_receive()`

**Receive data packet.**

```c
int akira_rf_receive(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);
```

**Parameters:**
- `buffer` - Buffer to store received data
- `max_len` - Maximum buffer size
- `timeout_ms` - Receive timeout in milliseconds

**Returns:** Number of bytes received, negative on error

**Example:**
```c
uint8_t buffer[64];
int len = akira_rf_receive(buffer, sizeof(buffer), 1000);
if (len > 0) {
    printf("Received %d bytes!\n", len);
}
```

---

### `akira_rf_set_frequency()`

**Set RF frequency.**

```c
int akira_rf_set_frequency(uint32_t freq_hz);
```

**Parameters:**
- `freq_hz` - Frequency in Hz (e.g., `2450000000` for 2.45 GHz)

**Returns:** `0` on success

---

### `akira_rf_set_power()`

**Set transmit power.**

```c
int akira_rf_set_power(int8_t dbm);
```

**Parameters:**
- `dbm` - Power level in dBm (typically -18 to +20)

**Returns:** `0` on success

---

### `akira_rf_get_rssi()`

**Get RSSI of last received packet.**

```c
int akira_rf_get_rssi(int16_t *rssi);
```

**Parameters:**
- `rssi` - Output pointer for RSSI value in dBm

**Returns:** `0` on success

**Example:**
```c
int16_t rssi;
if (akira_rf_get_rssi(&rssi) == 0) {
    printf("Signal strength: %d dBm\n", rssi);
}
```

---

### `akira_rf_deinit()`

**Deinitialize RF chip.**

```c
int akira_rf_deinit(void);
```

**Returns:** `0` on success

---

## 🔌 GPIO & Timer API

**Capabilities Required:** `gpio.control`, `timer.control`

Event-driven hardware control with callbacks.

### Timer Callbacks

#### `akira_register_timer_callback()`

**Register a timer callback.**

```c
typedef void (*timer_callback_func_t)(void);
int akira_register_timer_callback(int timer_id, timer_callback_func_t callback);
```

**Parameters:**
- `timer_id` - Timer ID (0-15)
- `callback` - Function to call when timer fires

**Returns:** `0` on success, negative error code on failure

**Example:**
```c
void on_timer() {
    printf("Timer fired!\n");
}

akira_register_timer_callback(0, on_timer);
```

---

#### `akira_unregister_timer_callback()`

**Unregister a timer callback.**

```c
int akira_unregister_timer_callback(int timer_id);
```

**Parameters:**
- `timer_id` - Timer ID to unregister

**Returns:** `0` on success, `-ENOENT` if not registered

---

### GPIO Callbacks

#### `akira_register_gpio_callback()`

**Register a GPIO callback.**

```c
typedef void (*gpio_callback_func_t)(uint8_t state);
int akira_register_gpio_callback(gpio_callback_func_t callback, int port, int pin);
```

**Parameters:**
- `callback` - Function to call when GPIO state changes
- `port` - GPIO port number (0-7)
- `pin` - GPIO pin number (0-31)

**Returns:** `0` on success, `-ENOMEM` if no slots available

**Example:**
```c
void on_button_press(uint8_t state) {
    if (state) {
        printf("Button pressed!\n");
    }
}

akira_register_gpio_callback(on_button_press, 0, 5);
```

---

#### `akira_unregister_gpio_callback()`

**Unregister a GPIO callback.**

```c
int akira_unregister_gpio_callback(int port, int pin);
```

**Parameters:**
- `port` - GPIO port number
- `pin` - GPIO pin number

**Returns:** `0` on success, `-ENOENT` if not registered

---

### Message Callbacks

#### `akira_register_message_callback()`

**Register a message callback.**

```c
typedef void (*message_callback_func_t)(const char *topic,
                                        const char *content_type,
                                        const void *payload,
                                        uint32_t payload_len);

int akira_register_message_callback(message_callback_func_t callback,
                                    const char *topic);
```

**Parameters:**
- `callback` - Function to call when message is received
- `topic` - Topic string (max 127 chars)

**Returns:** `0` on success, `-ENOMEM` if no slots available

**Example:**
```c
void on_message(const char *topic, const char *content_type,
                const void *payload, uint32_t payload_len) {
    printf("Message on '%s': %u bytes\n", topic, payload_len);
}

akira_register_message_callback(on_message, "sensors/temp");
```

---

#### `akira_unregister_message_callback()`

**Unregister a message callback.**

```c
int akira_unregister_message_callback(const char *topic);
```

**Parameters:**
- `topic` - Topic to unregister

**Returns:** `0` on success, `-ENOENT` if not registered

---

## 📊 Sensor API

**Capability Required:** `sensor.<type>.read`

Read various sensor types with convenience functions for common sensor combinations.

### Sensor Types

```c
typedef enum {
    SENSOR_TYPE_NONE = 0,
    SENSOR_TYPE_ACCEL,      // Accelerometer
    SENSOR_TYPE_GYRO,       // Gyroscope
    SENSOR_TYPE_TEMP,       // Temperature
    SENSOR_TYPE_HUMIDITY,   // Humidity
    SENSOR_TYPE_PRESSURE,   // Barometric pressure
    SENSOR_TYPE_LIGHT,      // Light level
    SENSOR_TYPE_VOLTAGE,    // Voltage
    SENSOR_TYPE_CURRENT,    // Current
    SENSOR_TYPE_POWER       // Power
} akira_sensor_type_t;
```

---

### `akira_sensor_read()`

**Read single sensor value.**

```c
int akira_sensor_read(akira_sensor_type_t type, float *value);
```

**Parameters:**
- `type` - Sensor type
- `value` - Output pointer for sensor value

**Returns:** `0` on success, negative on error

**Example:**
```c
float temp;
if (akira_sensor_read(SENSOR_TYPE_TEMP, &temp) == 0) {
    printf("Temperature: %.1f°C\n", temp);
}
```

---

### `akira_sensor_read_imu()`

**Read IMU data (accelerometer + gyroscope).**

```c
typedef struct {
    float accel_x, accel_y, accel_z;  // m/s²
    float gyro_x, gyro_y, gyro_z;    // deg/s
} akira_imu_data_t;

int akira_sensor_read_imu(akira_imu_data_t *data);
```

**Parameters:**
- `data` - Output pointer for IMU data

**Returns:** `0` on success

**Example:**
```c
akira_imu_data_t imu;
if (akira_sensor_read_imu(&imu) == 0) {
    printf("Accel: %.2f, %.2f, %.2f\n",
           imu.accel_x, imu.accel_y, imu.accel_z);
}
```

---

### `akira_sensor_read_env()`

**Read environmental data (temp, humidity, pressure).**

```c
typedef struct {
    float temperature;  // °C
    float humidity;     // %
    float pressure;     // hPa
} akira_env_data_t;

int akira_sensor_read_env(akira_env_data_t *data);
```

**Parameters:**
- `data` - Output pointer for environmental data

**Returns:** `0` on success

---

### `akira_sensor_read_power()`

**Read power data (voltage, current, power).**

```c
typedef struct {
    float voltage;  // V
    float current;  // A
    float power;    // W
} akira_power_data_t;

int akira_sensor_read_power(akira_power_data_t *data);
```

**Parameters:**
- `data` - Output pointer for power data

**Returns:** `0` on success

---

## 💾 Storage API

**Capabilities Required:** `storage.read`, `storage.write`

Persistent file storage with read/write/delete operations.

### `akira_storage_read()`

**Read file from storage.**

```c
int akira_storage_read(const char *path, void *buffer, size_t len);
```

**Parameters:**
- `path` - File path (relative to app storage)
- `buffer` - Output buffer
- `len` - Maximum bytes to read

**Returns:** Bytes read on success, negative on error

**Example:**
```c
char data[100];
int bytes = akira_storage_read("config.txt", data, sizeof(data));
if (bytes > 0) {
    printf("Read %d bytes\n", bytes);
}
```

---

### `akira_storage_write()`

**Write file to storage.**

```c
int akira_storage_write(const char *path, const void *data, size_t len);
```

**Parameters:**
- `path` - File path
- `data` - Data to write
- `len` - Data length

**Returns:** Bytes written on success, negative on error

---

### `akira_storage_delete()`

**Delete file from storage.**

```c
int akira_storage_delete(const char *path);
```

**Returns:** `0` on success, negative on error

---

### `akira_storage_size()`

**Get file size.**

```c
int akira_storage_size(const char *path);
```

**Returns:** File size in bytes, negative on error

---

### `akira_storage_list()`

**List files in directory.**

```c
int akira_storage_list(const char *path, char **files, int max_count);
```

**Parameters:**
- `path` - Directory path
- `files` - Output array of file name pointers
- `max_count` - Maximum number of files

**Returns:** Number of files, negative on error

---

## 🌐 Network API

**Capabilities Required:** `network.http`, `network.mqtt`

HTTP and MQTT networking for IoT applications.

### HTTP Functions

#### `akira_http_get()`

**Perform HTTP GET request.**

```c
int akira_http_get(const char *url, uint8_t *buffer, size_t max_len);
```

**Parameters:**
- `url` - URL to fetch
- `buffer` - Response buffer
- `max_len` - Maximum response length

**Returns:** Response length on success, negative on error

**Example:**
```c
uint8_t response[1024];
int len = akira_http_get("http://api.example.com/data",
                         response, sizeof(response));
if (len > 0) {
    printf("Got %d bytes\n", len);
}
```

---

#### `akira_http_post()`

**Perform HTTP POST request.**

```c
int akira_http_post(const char *url, const uint8_t *data, size_t len);
```

**Parameters:**
- `url` - URL to post to
- `data` - Request body
- `len` - Request body length

**Returns:** Response code on success, negative on error

---

### MQTT Functions

#### `akira_mqtt_publish()`

**Publish MQTT message.**

```c
int akira_mqtt_publish(const char *topic, const void *data, size_t len);
```

**Parameters:**
- `topic` - Topic name
- `data` - Message data
- `len` - Data length

**Returns:** `0` on success, negative on error

**Example:**
```c
float temp = 23.5;
akira_mqtt_publish("home/sensor/temp", &temp, sizeof(temp));
```

---

#### `akira_mqtt_subscribe()`

**Subscribe to MQTT topic.**

```c
typedef void (*akira_mqtt_callback_t)(const char *topic,
                                      const void *data,
                                      size_t len);

int akira_mqtt_subscribe(const char *topic, akira_mqtt_callback_t callback);
```

**Parameters:**
- `topic` - Topic pattern (supports wildcards: `+` for single level, `#` for multi-level)
- `callback` - Function to call when message arrives

**Returns:** `0` on success, negative on error

**Example:**
```c
void on_mqtt_msg(const char *topic, const void *data, size_t len) {
    printf("MQTT on '%s': %zu bytes\n", topic, len);
}

akira_mqtt_subscribe("sensors/#", on_mqtt_msg);
```

---

## ⚙️ System API

**Capability Required:** `system.info`

System utilities and information.

### `akira_system_uptime_ms()`

**Get system uptime.**

```c
uint64_t akira_system_uptime_ms(void);
```

**Returns:** Uptime in milliseconds

---

### `akira_system_free_memory()`

**Get free heap memory.**

```c
size_t akira_system_free_memory(void);
```

**Returns:** Free bytes available

---

### `akira_system_platform()`

**Get platform name.**

```c
const char *akira_system_platform(void);
```

**Returns:** Platform string (e.g., "ESP32-S3")

---

### `akira_system_sleep()`

**Sleep for specified time.**

```c
void akira_system_sleep(uint32_t ms);
```

**Parameters:**
- `ms` - Milliseconds to sleep

---

### `akira_log()`

**Log message for debugging.**

```c
void akira_log(int level, const char *message);
```

**Parameters:**
- `level` - Log level (0=error, 1=warn, 2=info, 3=debug)
- `message` - Log message

**Example:**
```c
akira_log(2, "Application started!");
akira_log(1, "Low battery warning!");
akira_log(0, "Critical error!");
```

---

## 📏 Constants & Types

### Maximum Limits

```c
#define AKIRA_MAX_TIMERS                16
#define AKIRA_MAX_CALLBACKS             64
#define AKIRA_MAX_TOPIC_LEN             128
#define AKIRA_MAX_PAYLOAD_LEN           128
#define AKIRA_MAX_CONTENT_TYPE_LEN      64
#define AKIRA_MAX_GPIO_PORTS            8
#define AKIRA_MAX_GPIO_PINS_PER_PORT    32
#define AKIRA_MAX_GPIO_PINS             256
```

---

## Related Documentation

- [Native API (Low-Level)](../api-reference/native-api.md) - WAMR import/export signatures
- [Best Practices](best-practices.md) - Write efficient, maintainable code
- [SDK Troubleshooting](sdk-troubleshooting.md) - Debug common issues
- [Building Apps](building-apps.md) - WASM compilation workflow
- [First App Tutorial](../getting-started/first-app.md) - Hello World

---

[⬆ Back to Top](#-akira-sdk-api-reference)
