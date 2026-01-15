# AkiraOS v2.0 Architecture

**Universal Embedded Operating System for IoT, Wearables, and Edge Devices**

## Vision

AkiraOS is a modular, security-focused embedded OS designed for resource-constrained devices. Built on OCRE container runtime, it provides:

- **STUPID SIMPLE** — minimal complexity, direct initialization
- **Secure by Design** — capability-based security, signed apps, sandboxing
- **Modular** — use only what you need via Kconfig
- **Multi-Platform** — ESP32, nRF5x/nRF91, STM32 support
- **Driver Rich** — unified framework for displays, RF chips, sensors

## System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      WASM Applications                          │
│            (User apps compiled to WebAssembly)                  │
│        Trust Level 3: Sandboxed, restricted permissions         │
├─────────────────────────────────────────────────────────────────┤
│                         Akira API                               │
│     (Capability-based API: display, RF, sensors, storage)       │
├─────────────────────────────────────────────────────────────────┤
│                    OCRE Container Runtime                       │
│  ┌───────────────┬──────────────┬──────────────┬─────────────┐  │
│  │ App Lifecycle │  Security    │  Resource    │ IPC Message │  │
│  │   Manager     │  Enforcer    │  Scheduler   │     Bus     │  │
│  └───────────────┴──────────────┴──────────────┴─────────────┘  │
├─────────────────────────────────────────────────────────────────┤
│                     Subsystem Managers                          │
│  ┌──────────────┬──────────────┬──────────────┬──────────────┐  │
│  │ BT Manager   │ USB Manager  │ HID Manager  │ OTA Manager  │  │
│  │ (BLE stack)  │ (USB stack)  │ (Input dev)  │ (Updates)    │  │
│  └──────────────┴──────────────┴──────────────┴──────────────┘  │
│  ┌──────────────┬──────────────┬──────────────┬──────────────┐  │
│  │ FS Manager   │ App Manager  │Power Manager │Cloud Client  │  │
│  │ (Storage)    │ (App runtime)│ (Sleep)      │ (Messaging)  │  │
│  └──────────────┴──────────────┴──────────────┴──────────────┘  │
├─────────────────────────────────────────────────────────────────┤
│              Platform HAL (platform_hal)                        │
│        (Hardware GPIO, SPI, Display simulation)                 │
├─────────────────────────────────────────────────────────────────┤
│              Driver Registry (Runtime Driver Loading)           │
│        (Display, Sensors, RF, Storage drivers)                  │
├─────────────────────────────────────────────────────────────────┤
│                        Zephyr RTOS                              │
│   (Real-time kernel, networking, storage, USB, WiFi native)     │
├─────────────────────────────────────────────────────────────────┤
│              Hardware (ESP32 | nRF5x/nRF91 | STM32)             │
└─────────────────────────────────────────────────────────────────┘
```

### Initialization Flow (Direct & Simple)

**main.c** orchestrates system boot with direct function calls:

```c
1. Hardware Layer
   ├─ akira_hal_init()          // Platform HAL (GPIO, SPI, display)
   └─ driver_registry_init()    // Driver loading system

2. Storage (optional - #ifdef CONFIG_FILE_SYSTEM)
   └─ fs_manager_init()         // Filesystem

3. Settings (optional - #ifdef CONFIG_AKIRA_SETTINGS)
   └─ user_settings_init()      // Persistent settings

4. Network (optional - via Kconfig)
   ├─ Zephyr WiFi (native API)  // No wrapper needed
   ├─ bt_manager_init()         // Bluetooth stack
   └─ usb_manager_init()        // USB stack

5. Runtime (optional - #ifdef CONFIG_AKIRA_OCRE_RUNTIME)
   └─ ocre_runtime_init()       // WASM container runtime

6. Services (optional)
   ├─ app_manager_init()        // App lifecycle
   └─ akira_shell_init()        // Debug shell
```

**Design Principles:**
- No event bus or pub/sub complexity
- No init table with priority sorting
- No manager wrappers around existing APIs
- Use Zephyr's native stacks where well-designed (WiFi, networking)
- Direct calls with Kconfig for optional features
- Fail gracefully on optional features

## Trust Model

### Security Levels

| Level | Name | Access | Examples |
|-------|------|--------|----------|
| **Level 0** | Kernel | Direct hardware, all memory | Zephyr RTOS, HAL |
| **Level 1** | System Services | Privileged APIs, DMA | OTA Manager, RF Manager |
| **Level 2** | Trusted Apps | Extended permissions, signed | System utilities, certified apps |
| **Level 3** | User Apps | Sandboxed, capability-based | 3rd party WASM apps |

### App Signing & Verification

```
Developer → Signs .wasm → AkiraOS validates signature → OCRE loads app

Trust Chain:
1. Root CA (burned into device at manufacture)
2. Developer Certificate (signed by Root CA)
3. App Binary (signed by Developer Certificate)

OCRE verifies full chain before execution.
```

### App Manifest

**Optional but recommended.** Apps without manifest use defaults.

```json
{
  "name": "weather_station",
  "version": "1.0.0",
  "entry": "_start",
  "memory": {
    "heap_kb": 16,
    "stack_kb": 4
  },
  "restart": {
    "enabled": true,
    "max_retries": 3,
    "delay_ms": 1000
  },
  "permissions": ["display", "sensor", "network"]
}
```

| Field | Default |
|-------|---------|----------------------|
| name | filename |
| version | "0.0.0" |
| entry | "_start" |
| heap_kb | 16 |
| stack_kb | 4 |
| restart.enabled | false |
| permissions | none |

OCRE enforces permissions at runtime. Apps cannot call APIs outside granted capabilities.

---

## Initialization System

AkiraOS uses a **configuration-driven initialization** system that automatically starts only the features enabled in Kconfig.

### Init Table Architecture

The system uses a declarative initialization table with priorities:

```c
// src/core/init_table.c
typedef struct {
    const char *name;
    bool (*is_enabled)(void);      // Check Kconfig
    int (*init_func)(void);
    int (*start_func)(void);       // Optional post-init
    uint8_t priority;              // 0=highest
    bool required;                 // Fail if init fails
} subsystem_entry_t;

static const subsystem_entry_t init_table[] = {
    // Priority 0-9: Critical Foundation
    {"HAL",        always_enabled,    akira_hal_init,        NULL, 0, true},
    {"Drivers",    always_enabled,    driver_registry_init,  NULL, 1, true},
    {"Hardware",   hw_enabled,        hardware_manager_init, NULL, 2, true},
    
    // Priority 10-19: Core Services
    {"Storage",    storage_enabled,   fs_manager_init,       NULL, 10, true},
    {"Settings",   always_enabled,    user_settings_init,    NULL, 11, true},
    
    // Priority 20-29: Networking
    {"Network",    network_enabled,   network_manager_init,  network_manager_start, 20, false},
    
    // Priority 30-39: Applications
    {"Shell",      shell_enabled,     akira_shell_init,      NULL, 30, true},
    {"OTA",        ota_enabled,       ota_manager_init,      NULL, 31, false},
    {"WebServer",  http_enabled,      http_server_init,      http_server_start, 32, false},
    {"AppManager", app_mgr_enabled,   app_manager_init,      NULL, 33, false},
    
    {NULL} // Sentinel
};
```

### Main Entry Point

The refactored `main.c` is now minimal (< 100 lines):

```c
#include "core/system_manager.h"

int main(void) {
    LOG_INF("=== AkiraOS v%s Starting ===", CONFIG_AKIRA_VERSION);
    
    // Initialize system manager (reads Kconfig, loads settings)
    system_manager_init();
    
    // Start all enabled subsystems via init table
    system_manager_start();
    
    LOG_INF("=== AkiraOS Ready ===");
    
    // Run main event loop
    system_manager_run();
    
    return 0;
}
```

### Benefits

- ✅ **Config-driven**: Only enabled features are initialized
- ✅ **Dependency order**: Priority ensures correct initialization sequence
- ✅ **Graceful degradation**: Optional subsystems can fail without crashing
- ✅ **Maintainable**: Easy to add/remove subsystems
- ✅ **Testable**: Each subsystem can be tested in isolation

---

## App Manager

The App Manager handles WASM application installation, lifecycle, and resource management on top of OCRE.

### App Sources

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              EXTERNAL INTERFACES                             │
├─────────────┬─────────────┬─────────────┬─────────────┬─────────────────────┤
│   WiFi      │    BLE      │    USB      │  SD Card    │   Firmware Flash    │
│  (HTTP)     │  (GATT)     │   (MSC)     │   (SPI)     │     (Built-in)      │
└──────┬──────┴──────┬──────┴──────┬──────┴──────┬──────┴──────────┬──────────┘
       │             │             │             │                 │
       ▼             ▼             ▼             ▼                 ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           CONNECTIVITY LAYER                                 │
├─────────────────────┬───────────────────────┬───────────────────────────────┤
│   HTTP Server       │   BLE App Service     │   Storage Drivers             │
│   POST /api/apps    │   GATT Chunked TX     │   SD/USB Mount & Scan         │
└─────────┬───────────┴───────────┬───────────┴───────────┬───────────────────┘
          └───────────────────────┼───────────────────────┘
                                  ▼
                         ┌─────────────────┐
                         │  Install Manager │
                         │  WASM Validate   │
                         │  Manifest Parse  │
                         └────────┬────────┘
                                  ▼
                         ┌─────────────────┐
                         │  Core Registry  │
                         │  App Metadata   │
                         │  State Tracking │
                         └────────┬────────┘
                                  ▼
                         ┌─────────────────┐
                         │ Lifecycle Mgr   │
                         │ Start/Stop      │
                         │ Crash Recovery  │
                         └────────┬────────┘
                                  ▼
                            OCRE Runtime
```

| Source | Interface | Trigger | Notes |
|--------|-----------|---------|-------|
| HTTP OTA | WiFi | `POST /api/apps/install` | Chunked upload, progress callback |
| BLE Transfer | Bluetooth | GATT service 0xAK01 | 512B chunks, CRC32 validation |
| USB Storage | USB MSC | Mount event or `app scan /usb` | FAT32, manual confirmation |
| SD Card | SPI/SDIO | Boot scan or `app scan /sd` | FAT32, auto or manual install |
| Firmware | Flash | Boot | Read-only, cannot uninstall |

### Constraints

| Parameter | Value | Notes |
|-----------|-------|-------|
| Max installed apps | 8-16 | Based on flash size |
| Concurrent running | 1-2 | RAM constraints |
| Max app size | 64KB | Configurable per platform |
| Heap per app | 16KB | Default, adjustable in manifest |
| Stack per app | 4KB | Default, adjustable in manifest |

### App Lifecycle

```
                             ┌─────────────┐ restart 
                             │             │
                             ▼             │
┌────────┐   install   ┌───────────┐       │
│  NEW   │ ──────────► │ INSTALLED │       │
└────────┘             └─────┬─────┘       │
                             │ start       │
                             ▼             │
                run    ┌───────────┐       │
              ┌──────► │  RUNNING  │ ◄─────┤ recover (if enabled)
              │        └─────┬─────┘       │
              │ stop         │ crash       │
              ▼              ▼             │
        ┌───────────┐  ┌───────────┐       │
        │  STOPPED  │  │   ERROR   │ ──────┘
        └───────────┘  └─────┬─────┘
                             │ max retries exceeded
                             ▼
                       ┌───────────┐
                       │  FAILED   │
                       └───────────┘
```

| State | Description |
|-------|-------------|
| NEW | Being installed |
| INSTALLED | Ready to run |
| RUNNING | Currently executing |
| STOPPED | Manually stopped |
| ERROR | Crashed, pending restart |
| FAILED | Exceeded max restart retries |

### Crash Handling

- On crash: Log error, set state to ERROR, increment crash counter
- Auto-restart (if enabled): Wait delay, retry up to 3 times
- After max retries: State = FAILED, manual restart always allowed

---

## OCRE Container Runtime

### Core Responsibilities

1. **WASM Execution**
   - Load and validate WASM binaries
   - Initialize isolated execution context
   - AOT or interpreter mode (platform-dependent)

2. **Security Enforcement**
   - Memory isolation (WASM linear memory)
   - Capability-based permissions
   - Syscall filtering and validation

3. **Resource Management**
   - Memory quota enforcement (heap/stack limits)
   - CPU time slicing
   - Storage quota management

4. **Inter-Process Communication**
   - Message bus for async communication
   - Shared memory regions (with ACLs)
   - Event broadcasting

### Container Lifecycle

```
┌─────────┐    load     ┌─────────┐    start    ┌─────────┐
│ STOPPED │ ─────────→  │ LOADED  │ ──────────→ │ RUNNING │
└─────────┘             └─────────┘             └─────────┘
     ↑                       ↑                        │
     │                       │                        │ pause
     │ destroy               │ reload                 ↓
     │                       │                   ┌─────────┐
     └───────────────────────┴─────────────────  │ PAUSED  │
                                                 └─────────┘
```

### OCRE API

```c
// Container Management
int ocre_create_container(const char *name, const void *wasm_binary, 
                          size_t size, const ocre_manifest_t *manifest);
int ocre_start_container(const char *name);
int ocre_stop_container(const char *name);
int ocre_pause_container(const char *name);
int ocre_resume_container(const char *name);
int ocre_destroy_container(const char *name);

// Resource Control
int ocre_set_memory_limit(const char *name, size_t bytes);
int ocre_set_cpu_quota(const char *name, uint8_t percent);
int ocre_set_storage_limit(const char *name, size_t bytes);

// IPC
int ocre_send_message(const char *from, const char *to, 
                      const void *data, size_t len);
int ocre_subscribe_channel(const char *name, const char *channel);
int ocre_publish_event(const char *channel, const void *data, size_t len);
```

---

## Resource Limits & Quotas

### Per-Container Limits

| Resource | Default | Range | Enforcement |
|----------|---------|-------|-------------|
| **Memory** | 128KB | 64KB - 512KB | OCRE allocator |
| **CPU Time** | 10% | 5% - 50% | Scheduler quantum |
| **Storage** | 256KB | 64KB - 2MB | Filesystem quota |
| **Network** | 10KB/s | 1KB/s - 100KB/s | Rate limiter |
| **File Handles** | 8 | 4 - 32 | FD table limit |
| **Threads** | 2 | 1 - 4 | Thread pool |

### Scheduling Policy

```c
// Fair scheduling with CPU quotas
while (true) {
    for (container in active_containers) {
        time_slice = (container.cpu_quota / 100) * QUANTUM_MS;
        ocre_schedule_container(container, time_slice);
        
        if (container.exceeded_quota()) {
            ocre_throttle_container(container);
        }
    }
}
```

### Watchdog

- Each container has 5-second watchdog timer
- Blocks longer than 5s trigger `SIGKILL`
- Prevents runaway apps from freezing system

---

## Inter-App Communication (IPC)

### 1. Message Queue (Async)

```c
// Producer (App A)
ocre_publish_event("sensor/temperature", &temp_data, sizeof(temp_data));

// Consumer (App B)
ocre_subscribe_channel("sensor/*");
void on_message(const char *channel, const void *data, size_t len) {
    // Handle message
}
```

**Use case:** Sensor data distribution, notifications

### 2. Shared Memory (Zero-copy)

```c
// App A creates shared region
void *shm = ocre_shm_create("video_buffer", 320*240*2);
memcpy(shm, framebuffer, 320*240*2);

// App B attaches to region
void *shm = ocre_shm_attach("video_buffer");
display_draw(shm);
```

**Use case:** Large data transfer, video/audio buffers

**Security:** OCRE enforces ACLs per shared region

### 3. RPC (Sync)

```c
// App B exports function
OCRE_EXPORT int calculate_checksum(const uint8_t *data, size_t len);

// App A calls function
int result = ocre_rpc_call("app_b", "calculate_checksum", data, len);
```

**Use case:** Service APIs, function calls

### 4. Event Broadcasting

```c
// System events
ocre_broadcast_event("system/battery_low", &level, sizeof(level));
ocre_broadcast_event("system/button_pressed", &button_id, sizeof(button_id));

// Apps subscribe to system events
ocre_subscribe_channel("system/*");
```

**Use case:** System-wide notifications

---

## Akira API (WASM Exports)

### Display API

```c
// Exported to WASM
void akira_display_clear(uint16_t color);
void akira_display_pixel(int x, int y, uint16_t color);
void akira_display_rect(int x, int y, int w, int h, uint16_t color);
void akira_display_text(int x, int y, const char *text, uint16_t color);
void akira_display_flush(void);
```

**Permission required:** `display.write`

### Input API

```c
uint32_t akira_input_read_buttons(void);
void akira_input_set_callback(void (*callback)(uint32_t buttons));
```

**Permission required:** `input.read`

### RF API

```c
int akira_rf_init(enum rf_chip chip);
int akira_rf_send(const uint8_t *data, size_t len);
int akira_rf_receive(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);
int akira_rf_set_frequency(uint32_t freq_hz);
int akira_rf_set_power(int8_t dbm);
```

**Permission required:** `rf.transceive`

### Sensor API

```c
int akira_sensor_read(enum sensor_type type, float *value);
int akira_sensor_read_imu(akira_imu_data_t *data);
int akira_sensor_read_env(akira_env_data_t *data);
```

**Permission required:** `sensor.<type>.read`

### Storage API

```c
int akira_storage_read(const char *path, void *buffer, size_t len);
int akira_storage_write(const char *path, const void *data, size_t len);
int akira_storage_delete(const char *path);
int akira_storage_list(const char *path, char **files, int max_count);
```

**Permission required:** `storage.read` or `storage.write`

### Network API

```c
int akira_http_get(const char *url, uint8_t *buffer, size_t max_len);
int akira_http_post(const char *url, const uint8_t *data, size_t len);
int akira_mqtt_publish(const char *topic, const void *data, size_t len);
int akira_mqtt_subscribe(const char *topic, void (*callback)(const void *data, size_t len));
```

**Permission required:** `network.http` or `network.mqtt`

---

## Connectivity Layer

The connectivity layer provides unified interfaces for HID (Human Interface Device), HTTP server, Bluetooth, and USB functionality.

### HID Manager

AkiraOS can act as a HID device (keyboard, gamepad) over multiple transports:

```c
// Initialize HID with keyboard and gamepad profiles
hid_manager_init(HID_DEVICE_KEYBOARD | HID_DEVICE_GAMEPAD);

// Set transport (BLE, USB, or simulated for testing)
hid_manager_set_transport(HID_TRANSPORT_BLE);

// Send keyboard input
hid_keyboard_key_press(HID_KEY_A);
hid_keyboard_key_release(HID_KEY_A);

// Send gamepad input
hid_gamepad_button_press(HID_GAMEPAD_BUTTON_A);
hid_gamepad_axis_move(HID_GAMEPAD_AXIS_LEFT_X, 127);
```

### HID Configuration (Kconfig)

```kconfig
CONFIG_AKIRA_HID=y           # Enable HID subsystem
CONFIG_AKIRA_HID_KEYBOARD=y  # Keyboard profile
CONFIG_AKIRA_HID_GAMEPAD=y   # Gamepad profile
CONFIG_AKIRA_BT_HID=y        # HID over Bluetooth
CONFIG_AKIRA_USB_HID=y       # HID over USB
CONFIG_AKIRA_HID_SIM=y       # Simulation (native_sim)
```

### Bluetooth Manager

```c
// Initialize Bluetooth stack
akira_bt_init();

// Start advertising as HID device
akira_bt_advertise_start(AKIRA_BT_ADV_HID);

// Connection callbacks
akira_bt_register_connect_cb(on_connect);
akira_bt_register_disconnect_cb(on_disconnect);
```

### USB Manager

```c
// Initialize USB device stack
akira_usb_init();

// Enable HID class
akira_usb_enable_class(AKIRA_USB_CLASS_HID);
```

### HTTP Server

Independent HTTP server with WebSocket support:

```c
// Initialize and start server
akira_http_server_init();
akira_http_server_start();

// Register route handler
http_route_t route = {
    .method = HTTP_GET,
    .path = "/api/status",
    .handler = handle_status
};
akira_http_register_route(&route);

// Enable WebSocket
akira_http_enable_websocket();
akira_http_ws_register_message_cb(on_ws_message, NULL);

// Broadcast to all WebSocket clients
akira_http_ws_send_text(-1, "{\"event\":\"update\"}");
```

### HTTP Configuration (Kconfig)

```kconfig
CONFIG_AKIRA_HTTP_SERVER=y     # Enable HTTP server
CONFIG_AKIRA_HTTP_WEBSOCKET=y  # WebSocket support
CONFIG_AKIRA_HTTP_PORT=80      # Server port
CONFIG_AKIRA_HTTP_MAX_CONNECTIONS=4
```

### Cloud Client (Unified Messaging)

The Cloud Client provides unified message routing from **ALL external sources** to appropriate handlers:

**Message Sources:**
- **Cloud Server** — Remote WebSocket/CoAP/MQTT servers
- **AkiraApp (Bluetooth)** — Mobile companion app via BLE
- **Web Server** — Local HTTP/WebSocket interface
- **USB Host** — Connected computer via CDC ACM

**Message Handlers:**
- **OTA Handler** — Firmware updates from any source
- **App Handler** — WASM app downloads and updates
- **Data Handler** — Sensor data, telemetry, configuration
- **Custom Handlers** — User-defined message processors

```c
#include "cloud_client.h"
#include "cloud_app_handler.h"
#include "cloud_ota_handler.h"

// Initialize cloud connectivity
cloud_client_config_t config = {
    .server_url = "wss://cloud.akira.io/devices",
    .device_id = "akira-001",
    .heartbeat_interval_s = 30,
    .auto_reconnect = true
};
cloud_client_init(&config);
cloud_app_handler_init();
cloud_ota_handler_init();

// Connect to cloud
cloud_client_connect();

// Register custom handler
int my_handler(const cloud_msg_t *msg, void *user) {
    LOG_INF("Received: category=%d type=%d from=%d",
            msg->category, msg->type, msg->source);
    return 0;
}
cloud_register_handler(MSG_CAT_DATA, my_handler, NULL);

// Send message
cloud_msg_t msg = {
    .category = MSG_CAT_DATA,
    .type = MSG_TYPE_TELEMETRY,
    .payload = json_data,
    .payload_len = strlen(json_data)
};
cloud_client_send(&msg);
```

### Cloud Configuration (Kconfig)

```kconfig
CONFIG_AKIRA_CLOUD_CLIENT=y           # Enable cloud client
CONFIG_AKIRA_CLOUD_APP_HANDLER=y      # WASM app download handler
CONFIG_AKIRA_CLOUD_OTA_HANDLER=y      # Firmware update handler
CONFIG_AKIRA_CLOUD_SERVER_URL=""      # Default server URL
CONFIG_AKIRA_CLOUD_HEARTBEAT_SEC=30   # Heartbeat interval
CONFIG_AKIRA_CLOUD_AUTO_RECONNECT=y   # Auto-reconnect on disconnect
CONFIG_AKIRA_CLOUD_MSG_QUEUE_SIZE=16  # Message queue size
```

---

## Hardware Abstraction Layers

AkiraOS uses a two-layer HAL architecture:

### 1. Platform HAL (`src/drivers/platform_hal.h`)

Hardware-specific abstraction for GPIO, SPI, displays, and simulation:

```c
// GPIO operations
int platform_hal_gpio_init(void);
int platform_hal_gpio_set(uint32_t pin, int value);
int platform_hal_gpio_get(uint32_t pin);

// SPI operations
int platform_hal_spi_init(void);
int platform_hal_spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len);

// Display simulation (native_sim)
void platform_hal_display_sim_pixel(int x, int y, uint16_t color);
void platform_hal_display_sim_flush(void);

// Button simulation
uint32_t platform_hal_get_buttons(void);
```

### 2. System HAL (`src/akira/hal/hal.h`)

System-level abstraction for version, uptime, and device info:

```c
// System information
const char *akira_hal_get_version(void);
uint32_t akira_hal_get_uptime_ms(void);
const char *akira_hal_get_platform_name(void);

// System control
void akira_hal_reboot(void);
void akira_hal_shutdown(void);

// Device identification
const char *akira_hal_get_device_id(void);
```

### Supported Platforms

| Platform | MCU | Cores | RAM | Flash | Radios |
|----------|-----|-------|-----|-------|--------|
| **ESP32** | Xtensa LX6 | 2 | 520KB | 4MB+ | WiFi, BT |
| **ESP32-S3** | Xtensa LX7 | 2 | 512KB | 8MB+ | WiFi, BLE |
| **ESP32-C3** | RISC-V | 1 | 400KB | 4MB+ | WiFi, BLE |
| **nRF52840** | ARM M4F | 1 | 256KB | 1MB | BLE, 802.15.4 |
| **nRF5340** | ARM M33 | 2 | 512KB | 1MB | BLE, 802.15.4 |
| **nRF9160** | ARM M33 | 1 | 256KB | 1MB | LTE-M, NB-IoT |
| **STM32F4** | ARM M4 | 1 | 192KB | 1MB | - |
| **STM32L4** | ARM M4 | 1 | 128KB | 512KB | - |
| **STM32WB** | ARM M4/M0+ | 2 | 256KB | 1MB | BLE, 802.15.4 |

### HAL Interface

```c
// Platform detection
const char *akira_hal_get_platform(void);
bool akira_hal_has_wifi(void);
bool akira_hal_has_bluetooth(void);
bool akira_hal_has_lte(void);

// GPIO
int akira_gpio_configure(uint32_t pin, uint32_t flags);
int akira_gpio_set(uint32_t pin, int value);
int akira_gpio_get(uint32_t pin);

// SPI
int akira_spi_init(uint8_t bus, uint32_t freq);
int akira_spi_transfer(uint8_t bus, const void *tx, void *rx, size_t len);

// I2C
int akira_i2c_init(uint8_t bus, uint32_t freq);
int akira_i2c_write(uint8_t bus, uint16_t addr, const void *data, size_t len);
int akira_i2c_read(uint8_t bus, uint16_t addr, void *data, size_t len);

// UART
int akira_uart_init(uint8_t port, uint32_t baud);
int akira_uart_write(uint8_t port, const void *data, size_t len);
int akira_uart_read(uint8_t port, void *buffer, size_t max_len);

// ADC
int akira_adc_read(uint8_t channel, uint16_t *value);
```

---

## RF Driver Framework

### Unified RF API

All RF drivers implement the same interface:

```c
struct akira_rf_driver {
    const char *name;
    
    int (*init)(void);
    int (*deinit)(void);
    int (*tx)(const uint8_t *data, size_t len);
    int (*rx)(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);
    int (*set_frequency)(uint32_t freq_hz);
    int (*set_power)(int8_t dbm);
    int (*set_mode)(enum rf_mode mode);
    int (*get_rssi)(int16_t *rssi);
};
```

### Supported RF Chips

| Chip | Freq Range | Modulation | Use Case |
|------|------------|------------|----------|
| **LR1121** | 150-960 MHz | LoRa, GFSK | LoRaWAN, satellite |
| **LR1210** | 150-960 MHz | LoRa, GFSK | Long-range IoT |
| **CC1101** | 300-928 MHz | FSK, GFSK, MSK, OOK | Sub-GHz proprietary |
| **nRF24L01** | 2.4 GHz | GFSK | Short-range |
| **SX1276** | 137-1020 MHz | LoRa, FSK | LoRaWAN |
| **RFM69** | 315/433/868/915 MHz | FSK, GFSK | Sensor networks |

### Example Usage

```c
// App selects RF chip at runtime
akira_rf_init(RF_CHIP_LR1121);
akira_rf_set_frequency(868100000); // 868.1 MHz
akira_rf_set_power(14); // 14 dBm

// Send LoRa packet
uint8_t packet[] = {0x01, 0x02, 0x03};
akira_rf_send(packet, sizeof(packet));

// Receive with timeout
uint8_t buffer[256];
int len = akira_rf_receive(buffer, sizeof(buffer), 5000);
```

---

## Driver Framework

### Display Drivers

| Driver | Resolution | Interface | Colors |
|--------|------------|-----------|--------|
| **ILI9341** | 320x240 | SPI | 16-bit RGB565 |
| **ST7789** | 240x320 | SPI | 16-bit RGB565 |
| **SSD1306** | 128x64 | I2C/SPI | Monochrome |
| **E-Paper** | Various | SPI | 1/4 bit grayscale |
| **Sharp Memory LCD** | 400x240 | SPI | Monochrome |

### Sensor Drivers

| Sensor | Type | Interface | Data |
|--------|------|-----------|------|
| **LSM6DS3** | IMU | I2C/SPI | Accel, Gyro |
| **MPU6050** | IMU | I2C | Accel, Gyro |
| **BME280** | Environmental | I2C/SPI | Temp, Humidity, Pressure |
| **INA219** | Power | I2C | Voltage, Current, Power |
| **BMP280** | Pressure | I2C/SPI | Pressure, Temp |
| **VEML7700** | Light | I2C | Ambient light |

### Storage Drivers

- **LittleFS** — wear-leveling flash filesystem
- **NVS (ESP32)** — key-value storage
- **FAT (SD Card)** — external storage
- **EEPROM** — non-volatile settings

---

## OTA Manager

### Multi-Transport Architecture

OTA updates can be delivered through multiple transport sources:

```
┌─────────────────────────────────────────────────────────────────┐
│                       OTA Manager                               │
│            (Firmware validation, flashing, rollback)            │
├─────────────────────────────────────────────────────────────────┤
│                    OTA Transport Interface                      │
│  ┌──────────────┬──────────────┬──────────────┬──────────────┐ │
│  │ HTTP         │ BLE          │ USB          │ Cloud        │ │
│  │ Transport    │ Transport    │ Transport    │ Transport    │ │
│  │ (Web upload) │ (GATT OTA)   │ (CDC ACM)    │ (AkiraHub)   │ │
│  └──────────────┴──────────────┴──────────────┴──────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### Transport Selection

```c
// Enable/disable OTA sources at runtime
ota_transport_set_enabled_sources(OTA_SOURCE_HTTP | OTA_SOURCE_BLE);

// Register callbacks for progress and completion
ota_transport_set_data_cb(on_ota_data);
ota_transport_set_complete_cb(on_ota_complete);
```

### OTA Source Configuration (Kconfig)

```kconfig
CONFIG_AKIRA_OTA=y
CONFIG_AKIRA_OTA_HTTP=y      # Via web server upload
CONFIG_AKIRA_OTA_BLE=y       # Via Bluetooth
CONFIG_AKIRA_OTA_USB=y       # Via USB connection
CONFIG_AKIRA_OTA_CLOUD=n     # Cloud (future AkiraHub)
```

### Secure Update Process

```
1. Client → Requests update from server (HTTPS/CoAP/BLE)
2. OTA Manager → Downloads firmware image
3. Verify signature → RSA-2048 or Ed25519
4. Verify hash → SHA-256 checksum
5. Write to secondary slot → MCUboot partition
6. Set boot flag → "image_ok" in MCUboot
7. Reboot → MCUboot swaps partitions
8. New firmware boots → Validates and confirms
9. If boot fails → MCUboot rolls back
```

### Transport Methods

| Transport | Use Case | Security |
|-----------|----------|----------|
| **HTTPS** | WiFi devices | TLS 1.3 |
| **CoAP** | Constrained networks | DTLS 1.2 |
| **Bluetooth** | Local updates | Encrypted BLE |
| **UART** | Factory programming | None (physical access) |

### API

```c
int ota_manager_init(void);
int ota_start_update(const char *url, const uint8_t *signature);
int ota_write_chunk(const uint8_t *data, size_t len);
int ota_finalize_update(void);
int ota_abort_update(void);
int ota_confirm_firmware(void); // Prevent rollback
int ota_rollback_request(void); // Force rollback
```

---

## Power Manager

### Power Modes

| Mode | CPU | RAM | Peripherals | Wake Sources |
|------|-----|-----|-------------|--------------|
| **Active** | On | On | On | N/A |
| **Idle** | Clock gated | On | On | Any interrupt |
| **Light Sleep** | Off | On | Selected | GPIO, Timer, RTC |
| **Deep Sleep** | Off | Off (RTC only) | Off | GPIO, Timer, ULP |
| **Hibernate** | Off | Off | Off | RTC timer only |

### API

```c
int akira_pm_set_mode(enum power_mode mode);
int akira_pm_wake_on_gpio(uint32_t pin, int edge);
int akira_pm_wake_on_timer(uint32_t ms);
int akira_pm_get_battery_level(uint8_t *percent);
int akira_pm_enable_low_power_mode(bool enable);
```

### Power Policies

Apps can request power preferences:

```c
// App declares it needs low latency
ocre_set_power_policy("my_app", POWER_POLICY_PERFORMANCE);

// App declares it's idle
ocre_set_power_policy("my_app", POWER_POLICY_LOW_POWER);
```

OCRE aggregates all app policies and selects optimal system power mode.

---

## Build System

### Build Targets

```bash
# ESP32 family
west build -b esp32_devkitc_wroom AkiraOS
west build -b esp32s3_devkitm AkiraOS
west build -b esp32c3_devkitm AkiraOS

# Nordic nRF family
west build -b nrf52840dk_nrf52840 AkiraOS
west build -b nrf5340dk_nrf5340_cpuapp AkiraOS
west build -b nrf9160dk_nrf9160 AkiraOS

# STM32 family
west build -b nucleo_f446re AkiraOS
west build -b nucleo_l476rg AkiraOS
west build -b nucleo_wb55rg AkiraOS

# Native simulation
west build -b native_sim AkiraOS
```

### Configuration Files

| File | Purpose |
|------|---------|
| `prj.conf` | Zephyr kernel config |
| `boards/<board>.conf` | Board-specific config |
| `boards/<board>.overlay` | Device tree overlay |
| `ocre.conf` | OCRE runtime config |
| `CMakeLists.txt` | Build rules |

### Kconfig Options

```kconfig
# App Manager
CONFIG_AKIRA_APP_MANAGER=y
CONFIG_AKIRA_APP_MAX_INSTALLED=8
CONFIG_AKIRA_APP_MAX_RUNNING=2
CONFIG_AKIRA_APP_MAX_SIZE_KB=64
CONFIG_AKIRA_APP_DEFAULT_HEAP_KB=16
CONFIG_AKIRA_APP_DEFAULT_STACK_KB=4

# App Sources
CONFIG_AKIRA_APP_SOURCE_HTTP=y
CONFIG_AKIRA_APP_SOURCE_BLE=y
CONFIG_AKIRA_APP_SOURCE_USB=y
CONFIG_AKIRA_APP_SOURCE_SD=y
CONFIG_AKIRA_APP_SOURCE_FIRMWARE=y

# Auto-restart
CONFIG_AKIRA_APP_AUTO_RESTART=n
CONFIG_AKIRA_APP_MAX_RETRIES=3
CONFIG_AKIRA_APP_RESTART_DELAY_MS=1000

# OCRE Runtime
CONFIG_AKIRA_OCRE_RUNTIME=y
CONFIG_AKIRA_SECURITY_SIGNING=n
CONFIG_AKIRA_RF_FRAMEWORK=y
CONFIG_AKIRA_POWER_MANAGEMENT=y
```

---

## Memory Layout

### ESP32-S3 (8MB Flash)

```
┌────────────────────────────────────────┐
│  Bootloader (MCUboot)    │    64KB     │
│  Primary Slot            │   3.5MB     │
│  Secondary Slot (OTA)    │   3.5MB     │
│  LittleFS (Apps/Data)    │    896KB    │
│  NVS (Settings)          │    64KB     │
└────────────────────────────────────────┘

DRAM: 512KB
├─ Zephyr Kernel       ~60KB
├─ Network Stack       ~40KB
├─ OCRE Runtime        ~80KB
├─ WASM Heap           ~250KB
├─ App Containers      ~50KB
└─ Stack/Reserved      ~32KB
```

### nRF52840 (1MB Flash)

```
┌────────────────────────────────────────┐
│  Bootloader (MCUboot)    │    48KB     │
│  Primary Slot            │   450KB     │
│  Secondary Slot (OTA)    │   450KB     │
│  LittleFS (Apps/Data)    │    64KB     │
│  Settings                │    12KB     │
└────────────────────────────────────────┘

RAM: 256KB
├─ Zephyr Kernel       ~40KB
├─ BLE Stack           ~30KB
├─ OCRE Runtime        ~50KB
├─ WASM Heap           ~100KB
├─ App Containers      ~20KB
└─ Stack/Reserved      ~16KB
```

---

## Shell Commands

### System

```bash
sys info          # Platform, memory, uptime
sys stats         # CPU, memory usage
sys reboot        # Reboot device
sys sleep <ms>    # Enter sleep mode
```

### App Manager

```bash
app list                     # List all apps with state
app install /sd/myapp.wasm   # Install from path
app start <name>             # Start app
app stop <name>              # Stop app
app restart <name>           # Restart (resets crash counter)
app info <name>              # App details
app uninstall <name>         # Remove app
app scan /sd                 # Scan SD card for apps
app scan /usb                # Scan USB for apps
```

### Containers (OCRE)

```bash
ocre list                    # List all containers
ocre start <name>            # Start container
ocre stop <name>             # Stop container
ocre status <name>           # Container status
ocre resources <name>        # Resource usage
ocre logs <name>             # Container logs
```

### OTA

```bash
ota status           # Current firmware version
ota update <url>     # Download and install update
ota rollback         # Revert to previous version
ota confirm          # Confirm current firmware
```

### RF

```bash
rf list              # List available RF chips
rf init <chip>       # Initialize RF chip
rf send <hex>        # Send raw packet
rf recv              # Receive packet
rf freq <hz>         # Set frequency
rf power <dbm>       # Set TX power
```

### Sensors

```bash
sensor list          # List available sensors
sensor read <type>   # Read sensor value
sensor stream <type> # Stream sensor data
```

---

## Development Workflow

### 1. Create WASM App

```c
// my_app.c
#include "akira_api.h"

int main(void) {
    akira_display_clear(0x0000);
    akira_display_text(10, 10, "Hello AkiraOS!", 0xFFFF);
    akira_display_flush();
    return 0;
}
```

Compile to WASM:

```bash
clang --target=wasm32 -nostdlib -Wl,--no-entry -o my_app.wasm my_app.c
```

### 2. Create Manifest

```json
{
  "name": "my_app",
  "version": "1.0.0",
  "trust_level": 3,
  "capabilities": ["display.write"],
  "resources": {
    "max_memory_kb": 64,
    "max_cpu_percent": 10
  }
}
```

### 3. Sign App

```bash
akira-sign --key developer.key --cert developer.crt my_app.wasm
```

### 4. Deploy

```bash
# Via web interface
curl -X POST http://device.local/api/apps/upload \
  -F "wasm=@my_app.wasm" \
  -F "manifest=@manifest.json"

# Via shell
ocre load my_app my_app.wasm manifest.json
ocre start my_app
```

## License

GNU General Public License v3.0 — See `LICENSE` file.

## Contributing

See `CONTRIBUTING.md` for guidelines.