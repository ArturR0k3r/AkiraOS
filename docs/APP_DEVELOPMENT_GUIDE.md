 AkiraOS App Development Guide

**Complete guide to building WASM applications for AkiraOS**

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Development Setup](#development-setup)
3. [App Manifest Format](#app-manifest-format)
4. [API Reference](#api-reference)
5. [Example Apps](#example-apps)
6. [Building & Deploying](#building--deploying)
7. [Debugging](#debugging)
8. [Best Practices](#best-practices)

---

## Quick Start

### Prerequisites

```bash
# Install WASI SDK
wget https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-20/wasi-sdk-20.0-linux.tar.gz
tar xzf wasi-sdk-20.0-linux.tar.gz
export WASI_SDK_PATH=/path/to/wasi-sdk-20.0

# Or use Emscripten
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

### Hello World (5 minutes)

**Step 1:** Create `hello.c`

```c
#include "akira_api.h"

// Define colors (RGB565 format)
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F

void _start() {
    // Clear screen to black
    akira_display_clear(COLOR_BLACK);
    
    // Draw welcome message
    akira_display_text(10, 10, "Hello AkiraOS!", COLOR_CYAN);
    akira_display_text(10, 30, "WASM is running!", COLOR_GREEN);
    
    // Draw a rectangle
    akira_display_rect(50, 60, 100, 50, COLOR_MAGENTA);
    
    // Draw text inside rectangle
    akira_display_text(60, 75, "Box!", COLOR_WHITE);
}
```

**Step 2:** Create `hello.akapp` manifest

```toml
[app]
name = "hello_world"
version = "1.0.0"
author = "Your Name"
description = "Hello World demo"
binary = "hello.wasm"
icon = "icon.png"  # Optional

[capabilities]
display.write = true

[resources]
heap_kb = 64
stack_kb = 32
storage_quota_kb = 0
```

**Step 3:** Build

```bash
# Using WASI SDK
$WASI_SDK_PATH/bin/clang \
    -O2 \
    -I../AkiraOS/include \
    -Wl,--no-entry \
    -Wl,--export=_start \
    -o hello.wasm \
    hello.c

# Or use the helper script
cd AkiraOS
./scripts/build_wasm_app.sh hello.c
```

**Step 4:** Install & Run

```bash
# Via USB/Serial
west flash
# In AkiraOS shell:
app install hello.akapp
app start hello_world

# Or via WiFi OTA (if enabled)
curl -X POST http://akira-device.local/api/apps \
     -F "manifest=@hello.akapp" \
     -F "binary=@hello.wasm"
```

---

## Development Setup

### Project Structure

```
my_akira_app/
├── src/
│   └── main.c              # Your app code
├── include/
│   └── akira_api.h         # Copy from AkiraOS/include/api/
├── assets/
│   ├── icon.png            # 64x64 app icon
│   └── data.bin            # Any assets to bundle
├── manifest.akapp          # App metadata
├── build.sh                # Build script
└── README.md
```

### Recommended Toolchain

**Option A: WASI SDK (Recommended)**
- Smaller binaries
- Better WASM compatibility
- Simpler setup

**Option B: Emscripten**
- More features (pthreads, SIMD)
- Better C++ support
- Larger binaries

**Option C: Rust + wasm32-wasi**
```bash
rustup target add wasm32-wasi
cargo build --target wasm32-wasi --release
```

---

## App Manifest Format

### Complete Example

```toml
# manifest.akapp

[app]
name = "my_app"               # Unique identifier (lowercase, underscores)
version = "1.2.3"             # Semantic versioning
author = "Jane Developer"
email = "jane@example.com"
description = "A cool app"
license = "MIT"
binary = "app.wasm"
icon = "icon.png"
url = "https://github.com/user/app"

# App behavior
auto_start = false            # Start on boot
auto_restart = true           # Restart on crash
priority = 5                  # Scheduler priority (0-10)

[capabilities]
# Display
display.read = false          # Read framebuffer
display.write = true          # Draw to screen

# Input
input.read = true             # Poll buttons/touch
input.callback = true         # Register input handler

# Storage
storage.read = true           # Read files
storage.write = true          # Write files

# Network
network.http = true           # HTTP client
network.mqtt = false          # MQTT client
network.raw = false           # Raw sockets (dangerous!)

# Sensors
sensor.imu = true             # Accelerometer/gyro
sensor.env = false            # Temperature/humidity
sensor.power = true           # Battery status
sensor.light = false          # Ambient light

# RF (Advanced)
rf.init = false               # Initialize RF chips
rf.transceive = false         # Send/receive RF
rf.config = false             # Configure RF parameters

# Bluetooth
bt.advertise = false          # BLE advertising
bt.connect = false            # BLE connections
bt.hid = false                # HID profile (keyboard/gamepad)

# System (Privileged)
system.info = true            # Read system info
system.reboot = false         # Reboot device
system.settings = false       # Modify system settings

# IPC
ipc.send = true               # Send messages to other apps
ipc.receive = true            # Receive messages
ipc.shm = false               # Shared memory

[resources]
heap_kb = 128                 # WASM linear memory
stack_kb = 64                 # WASM stack
storage_quota_kb = 512        # Max persistent storage
cpu_quota_ms = 100            # Max CPU time per tick

[signature]
# Generated by signing tool
algorithm = "RSA-2048-SHA256"
signature = "base64_encoded_signature"
cert_chain = ["cert1.pem", "cert2.pem", "root.pem"]
```

### Minimal Example

```toml
[app]
name = "simple_app"
version = "1.0.0"
binary = "app.wasm"

[capabilities]
display.write = true
input.read = true

[resources]
heap_kb = 64
stack_kb = 32
```

---

## API Reference

### Display API

```c
// Colors (RGB565 format)
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

// Clear entire screen
void akira_display_clear(uint16_t color);

// Draw single pixel
void akira_display_pixel(int x, int y, uint16_t color);

// Draw filled rectangle
void akira_display_rect(int x, int y, int w, int h, uint16_t color);

// Draw text (7x10 font)
void akira_display_text(int x, int y, const char *text, uint16_t color);

// Draw large text (11x18 font)
void akira_display_text_large(int x, int y, const char *text, uint16_t color);

// Flush to display (if double-buffered)
void akira_display_flush(void);

// Get display dimensions
void akira_display_get_size(int *width, int *height);
```

### Input API

```c
// Button states
typedef enum {
    BTN_A = 0,
    BTN_B = 1,
    BTN_X = 2,
    BTN_Y = 3,
    BTN_UP = 4,
    BTN_DOWN = 5,
    BTN_LEFT = 6,
    BTN_RIGHT = 7,
    BTN_START = 8,
    BTN_SELECT = 9,
} button_t;

// Check if button is currently pressed
bool akira_input_button_pressed(button_t btn);

// Wait for button press (blocking)
button_t akira_input_wait_button(void);

// Register callback for button events
typedef void (*button_callback_t)(button_t btn, bool pressed);
void akira_input_register_callback(button_callback_t cb);

// Touch input (if available)
typedef struct {
    int x, y;
    bool touched;
} touch_point_t;
bool akira_input_get_touch(touch_point_t *point);
```

### Storage API

```c
// Read file into buffer
// Returns: bytes read or negative error code
int akira_storage_read(const char *path, void *buffer, size_t len);

// Write buffer to file
// Returns: bytes written or negative error code
int akira_storage_write(const char *path, const void *data, size_t len);

// Delete file
// Returns: 0 on success, negative on error
int akira_storage_delete(const char *path);

// List files in directory
// Returns: number of files or negative error code
int akira_storage_list(const char *path, char **files, int max_count);

// Get file size
// Returns: size in bytes or negative error code
int akira_storage_size(const char *path);

// Check if file exists
bool akira_storage_exists(const char *path);
```

### Sensor API

```c
// IMU data (accelerometer + gyroscope)
typedef struct {
    float accel_x, accel_y, accel_z;  // m/s²
    float gyro_x, gyro_y, gyro_z;     // deg/s
    float temp;                        // °C
} imu_data_t;

int akira_sensor_imu_read(imu_data_t *data);

// Environmental sensors
typedef struct {
    float temperature;  // °C
    float humidity;     // %
    float pressure;     // hPa
} env_data_t;

int akira_sensor_env_read(env_data_t *data);

// Power/battery status
typedef struct {
    float voltage;      // V
    float current;      // mA
    int percent;        // 0-100%
    bool charging;
} power_data_t;

int akira_sensor_power_read(power_data_t *data);
```

### Network API

```c
// HTTP client
typedef struct {
    char *url;
    char *method;  // "GET", "POST", etc.
    char *headers; // "Key: Value\nKey: Value"
    char *body;
    size_t body_len;
} http_request_t;

typedef struct {
    int status_code;
    char *headers;
    char *body;
    size_t body_len;
} http_response_t;

int akira_network_http_request(http_request_t *req, http_response_t *resp);

// MQTT client
int akira_network_mqtt_connect(const char *broker, int port);
int akira_network_mqtt_publish(const char *topic, const void *data, size_t len);
int akira_network_mqtt_subscribe(const char *topic);
```

### System API

```c
// Get system info
typedef struct {
    char version[32];       // "AkiraOS v2.0.1"
    uint32_t uptime_sec;    // Seconds since boot
    uint32_t free_heap_kb;  // Available memory
    int battery_percent;    // 0-100
    bool wifi_connected;
    char ip_address[16];    // "192.168.1.100"
} system_info_t;

void akira_system_get_info(system_info_t *info);

// Time functions
uint64_t akira_system_get_time_ms(void);  // Milliseconds since boot
void akira_system_sleep_ms(uint32_t ms);  // Sleep for N milliseconds

// Logging
void akira_log_debug(const char *msg);
void akira_log_info(const char *msg);
void akira_log_warn(const char *msg);
void akira_log_error(const char *msg);

// Reboot (requires system.reboot capability)
void akira_system_reboot(void);
```

### IPC API

```c
// Send message to another app
int akira_ipc_send(const char *app_name, const void *data, size_t len);

// Receive message (blocking)
int akira_ipc_receive(char *sender, void *buffer, size_t max_len);

// Check for pending messages
bool akira_ipc_has_message(void);
```

---

## Example Apps

### Example 1: Hello World

See [Quick Start](#quick-start) above.

### Example 2: Button Input Demo

```c
#include "akira_api.h"

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_RED   0xF800
#define COLOR_GREEN 0x07E0

void _start() {
    akira_display_clear(COLOR_BLACK);
    akira_display_text(10, 10, "Press buttons!", COLOR_WHITE);
    
    int y = 40;
    
    while (true) {
        if (akira_input_button_pressed(BTN_A)) {
            akira_display_text(10, y, "A pressed", COLOR_GREEN);
            y += 15;
        }
        
        if (akira_input_button_pressed(BTN_B)) {
            akira_display_text(10, y, "B pressed", COLOR_RED);
            y += 15;
        }
        
        if (y > 220) {
            y = 40;
            akira_display_rect(0, 40, 320, 200, COLOR_BLACK);
        }
        
        akira_system_sleep_ms(50);
    }
}
```

**Manifest:**
```toml
[app]
name = "button_demo"
version = "1.0.0"
binary = "button_demo.wasm"

[capabilities]
display.write = true
input.read = true
```

### Example 3: Storage Demo

```c
#include "akira_api.h"
#include <string.h>

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_GREEN 0x07E0

void _start() {
    akira_display_clear(COLOR_BLACK);
    akira_display_text(10, 10, "Storage Demo", COLOR_WHITE);
    
    // Write a file
    const char *data = "Hello from WASM!";
    int ret = akira_storage_write("test.txt", data, strlen(data));
    
    if (ret > 0) {
        akira_display_text(10, 30, "Write OK", COLOR_GREEN);
    }
    
    // Read it back
    char buffer[64] = {0};
    ret = akira_storage_read("test.txt", buffer, sizeof(buffer) - 1);
    
    if (ret > 0) {
        akira_display_text(10, 50, "Read OK:", COLOR_GREEN);
        akira_display_text(10, 70, buffer, COLOR_WHITE);
    }
    
    // Get file size
    int size = akira_storage_size("test.txt");
    char size_str[32];
    snprintf(size_str, sizeof(size_str), "Size: %d bytes", size);
    akira_display_text(10, 90, size_str, COLOR_WHITE);
}
```

**Manifest:**
```toml
[app]
name = "storage_demo"
version = "1.0.0"
binary = "storage_demo.wasm"

[capabilities]
display.write = true
storage.read = true
storage.write = true

[resources]
storage_quota_kb = 64
```

### Example 4: Sensor Reader

```c
#include "akira_api.h"
#include <stdio.h>

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_CYAN  0x07FF

void _start() {
    akira_display_clear(COLOR_BLACK);
    akira_display_text(10, 10, "Sensor Monitor", COLOR_CYAN);
    
    while (true) {
        // Clear sensor area
        akira_display_rect(0, 40, 320, 200, COLOR_BLACK);
        
        // Read IMU
        imu_data_t imu;
        if (akira_sensor_imu_read(&imu) == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Accel: %.2f %.2f %.2f", 
                     imu.accel_x, imu.accel_y, imu.accel_z);
            akira_display_text(10, 50, buf, COLOR_WHITE);
            
            snprintf(buf, sizeof(buf), "Gyro: %.2f %.2f %.2f", 
                     imu.gyro_x, imu.gyro_y, imu.gyro_z);
            akira_display_text(10, 70, buf, COLOR_WHITE);
        }
        
        // Read power status
        power_data_t power;
        if (akira_sensor_power_read(&power) == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Battery: %d%% (%.2fV)", 
                     power.percent, power.voltage);
            akira_display_text(10, 100, buf, power.charging ? 0x07E0 : COLOR_WHITE);
        }
        
        akira_system_sleep_ms(100);
    }
}
```

**Manifest:**
```toml
[app]
name = "sensor_monitor"
version = "1.0.0"
binary = "sensor_monitor.wasm"

[capabilities]
display.write = true
sensor.imu = true
sensor.power = true
```

### Example 5: Network Client

```c
#include "akira_api.h"
#include <string.h>

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_GREEN 0x07E0
#define COLOR_RED   0xF800

void _start() {
    akira_display_clear(COLOR_BLACK);
    akira_display_text(10, 10, "HTTP Client", COLOR_WHITE);
    
    // Make HTTP request
    http_request_t req = {
        .url = "http://api.example.com/data",
        .method = "GET",
        .headers = "User-Agent: AkiraOS\nAccept: application/json",
        .body = NULL,
        .body_len = 0
    };
    
    http_response_t resp = {0};
    
    akira_display_text(10, 40, "Fetching...", COLOR_WHITE);
    
    int ret = akira_network_http_request(&req, &resp);
    
    if (ret == 0 && resp.status_code == 200) {
        akira_display_text(10, 60, "Success!", COLOR_GREEN);
        
        // Display first 100 chars of response
        char preview[101] = {0};
        strncpy(preview, resp.body, 100);
        
        int y = 80;
        char *line = strtok(preview, "\n");
        while (line && y < 220) {
            akira_display_text(10, y, line, COLOR_WHITE);
            y += 15;
            line = strtok(NULL, "\n");
        }
    } else {
        akira_display_text(10, 60, "Failed!", COLOR_RED);
        
        char err[32];
        snprintf(err, sizeof(err), "Status: %d", resp.status_code);
        akira_display_text(10, 80, err, COLOR_RED);
    }
}
```

**Manifest:**
```toml
[app]
name = "http_client"
version = "1.0.0"
binary = "http_client.wasm"

[capabilities]
display.write = true
network.http = true
```

---

## Building & Deploying

### Build Script

Create `build.sh`:

```bash
#!/bin/bash
set -e

APP_NAME="my_app"
WASI_SDK=/path/to/wasi-sdk-20.0

# Compile to WASM
$WASI_SDK/bin/clang \
    -O2 \
    -I../AkiraOS/include \
    -Wl,--no-entry \
    -Wl,--export=_start \
    -Wl,--strip-all \
    -o ${APP_NAME}.wasm \
    src/*.c

# Optimize with wasm-opt (optional)
if command -v wasm-opt &> /dev/null; then
    wasm-opt -Oz ${APP_NAME}.wasm -o ${APP_NAME}_opt.wasm
    mv ${APP_NAME}_opt.wasm ${APP_NAME}.wasm
fi

# Show size
ls -lh ${APP_NAME}.wasm

echo "✓ Build complete: ${APP_NAME}.wasm"
```

### Installation Methods

#### Method 1: USB/Serial Upload

```bash
# Flash AkiraOS
west flash

# Connect to shell
minicom -D /dev/ttyUSB0 -b 115200

# In shell:
app install /sd/apps/my_app.akapp
app start my_app
```

#### Method 2: WiFi OTA

```bash
# Using curl
curl -X POST http://akira-device.local/api/apps \
     -F "manifest=@manifest.akapp" \
     -F "binary=@app.wasm"

# Using web UI
# Open http://akira-device.local in browser
# Navigate to Apps → Install
# Upload manifest + WASM file
```

#### Method 3: SD Card

```bash
# Copy to SD card
cp my_app.akapp /media/sdcard/
cp my_app.wasm /media/sdcard/

# In AkiraOS shell:
app install /sd/my_app.akapp
```

### Signing Apps

```bash
# Generate key pair (one-time)
openssl genrsa -out app_key.pem 2048
openssl rsa -in app_key.pem -pubout -out app_key_pub.pem

# Sign app
./scripts/sign_app.sh app.wasm app_key.pem

# This updates manifest.akapp with signature
```

---

## Debugging

### Serial Logging

Add logging to your app:

```c
akira_log_info("App started");
akira_log_debug("Button pressed");
akira_log_warn("Low memory");
akira_log_error("Failed to read sensor");
```

View logs over serial:

```bash
minicom -D /dev/ttyUSB0 -b 115200
# or
screen /dev/ttyUSB0 115200
```

### App Shell Commands

```bash
# List installed apps
app list

# Get app info
app info my_app

# View app logs
app logs my_app

# Start/stop app
app start my_app
app stop my_app

# Restart app
app restart my_app

# Uninstall app
app uninstall my_app
```

### WASM Debugging with wasmtime

Test locally before deploying:

```bash
# Install wasmtime
curl https://wasmtime.dev/install.sh -sSf | bash

# Run app locally (mock API)
wasmtime run --dir=. app.wasm
```

### Common Issues

**Issue:** App crashes immediately

```bash
# Check logs
app logs my_app

# Common causes:
# - Forgot to export _start symbol
# - Stack overflow (increase stack_kb in manifest)
# - Null pointer dereference
```

**Issue:** API calls fail

```bash
# Check capabilities in manifest
app info my_app

# Make sure required capabilities are granted
# Example: display.write = true
```

**Issue:** File not found

```bash
# List app storage
storage list my_app

# Check path (relative paths only)
# Good: "data/config.txt"
# Bad:  "/data/config.txt"
# Bad:  "../other_app/data.txt"
```

---

## Best Practices

### Performance

1. **Minimize API calls in tight loops**
   ```c
   // Bad
   for (int i = 0; i < 1000; i++) {
       akira_display_pixel(i, 100, color);
   }
   
   // Good
   akira_display_rect(0, 100, 1000, 1, color);
   ```

2. **Use buffering for storage**
   ```c
   // Bad: Write byte by byte
   for (int i = 0; i < 1000; i++) {
       akira_storage_write("log.txt", &data[i], 1);
   }
   
   // Good: Write once
   akira_storage_write("log.txt", data, 1000);
   ```

3. **Request minimal capabilities**
   - Only request capabilities you actually use
   - Security & performance overhead for each capability

### Security

1. **Validate all inputs**
   ```c
   button_t btn = akira_input_wait_button();
   if (btn < 0 || btn > BTN_SELECT) {
       return; // Invalid button
   }
   ```

2. **Never trust file sizes**
   ```c
   int size = akira_storage_size("user_data.txt");
   if (size < 0 || size > MAX_FILE_SIZE) {
       akira_log_error("Invalid file size");
       return;
   }
   ```

3. **Sanitize displayed text**
   - Limit string lengths
   - Check for null terminators

### Memory Management

1. **Use static allocation when possible**
   ```c
   // Good for embedded
   char buffer[256];
   
   // Avoid if possible (requires heap)
   char *buffer = malloc(256);
   ```

2. **Keep heap usage low**
   - Default heap: 64KB
   - Monitor with `akira_system_get_info()`

3. **Avoid deep recursion**
   - Limited stack (32-64KB)
   - Prefer iteration over recursion

### Battery Life

1. **Sleep when idle**
   ```c
   while (true) {
       // Do work
       process_frame();
       
       // Sleep to save power
       akira_system_sleep_ms(16); // 60 FPS
   }
   ```

2. **Minimize display updates**
   - Update only changed areas
   - Use `akira_display_flush()` wisely

3. **Batch sensor reads**
   ```c
   // Read sensors every second, not every frame
   if (frame % 60 == 0) {
       akira_sensor_imu_read(&imu);
   }
   ```

---

## Advanced Topics

### Multi-threading (Coming Soon)

AkiraOS will support WASM threads via wasm32-wasi-threads:

```c
#include <pthread.h>

void *worker(void *arg) {
    // Background work
    return NULL;
}

void _start() {
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    
    // Main loop
    while (true) {
        // Update display
        akira_system_sleep_ms(16);
    }
}
```

### Graphics Library Integration

Use 2D graphics libraries:

```bash
# SDL2 (via Emscripten)
emcc -O2 -s USE_SDL=2 game.c -o game.wasm

# Raylib (WASM support)
# See: https://github.com/raysan5/raylib
```

### Custom Fonts

Include TTF fonts in your app:

```c
// TODO: Font API coming soon
void akira_display_load_font(const void *ttf_data, size_t len);
void akira_display_text_ttf(int x, int y, const char *text, int size, uint16_t color);
```

---

## Resources

- **AkiraOS GitHub:** https://github.com/ArturR0k3r/AkiraOS
- **API Headers:** `AkiraOS/include/api/akira_api.h`
- **Example Apps:** `AkiraOS/samples/wasm_apps/`
- **WASI SDK:** https://github.com/WebAssembly/wasi-sdk
- **WASM Spec:** https://webassembly.org/

---

## Support

- **Issues:** https://github.com/ArturR0k3r/AkiraOS/issues
- **Discussions:** https://github.com/ArturR0k3r/AkiraOS/discussions
- **Discord:** [Coming Soon]

---

**Happy Coding! 🚀**
