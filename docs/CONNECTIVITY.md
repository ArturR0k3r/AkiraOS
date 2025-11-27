# AkiraOS Connectivity Layer

This document describes the connectivity subsystem architecture for AkiraOS.

## Overview

The connectivity layer provides unified interfaces for:

- **HID (Human Interface Device)** - Act as keyboard/gamepad over BLE or USB
- **HTTP Server** - Web interface with WebSocket support
- **Bluetooth Manager** - BLE stack management and HID service
- **USB Manager** - USB device stack and HID class
- **OTA Transports** - Multi-source firmware updates

## Directory Structure

```
src/connectivity/
├── hid/
│   ├── hid_common.h      # Common HID types, keycodes, report structures
│   ├── hid_manager.h     # Unified HID manager API
│   ├── hid_manager.c     # HID manager with transport abstraction
│   ├── hid_sim.h         # Simulation transport for native_sim
│   └── hid_sim.c         # Logs HID events for testing
├── bluetooth/
│   ├── bt_manager.h      # Bluetooth stack management
│   ├── bt_manager.c      # Advertising, connections, pairing
│   ├── bt_hid.h          # BLE HID service API
│   └── bt_hid.c          # BLE HID implementation
├── usb/
│   ├── usb_manager.h     # USB device stack management
│   ├── usb_manager.c     # Device enumeration, class control
│   ├── usb_hid.h         # USB HID device API
│   └── usb_hid.c         # USB HID implementation
├── http/
│   ├── http_server.h     # HTTP server with WebSocket
│   └── http_server.c     # Route handling, WebSocket management
└── client/
    ├── ws_client.h       # WebSocket client API
    ├── ws_client.c       # WebSocket client implementation
    ├── coap_client.h     # CoAP client API
    └── coap_client.c     # CoAP client implementation
```

## HID Subsystem

### Features

- **Keyboard Profile**: Standard USB/BLE keyboard with all keycodes
- **Gamepad Profile**: 16 buttons, 6 axes, D-pad/hat switch
- **Transport Abstraction**: Same API works over BLE, USB, or simulation
- **Device Simulation**: Test HID functionality on native_sim

### Usage

```c
#include "hid_manager.h"

// Initialize with keyboard and gamepad
hid_manager_init(HID_DEVICE_KEYBOARD | HID_DEVICE_GAMEPAD);

// Select transport
hid_manager_set_transport(HID_TRANSPORT_BLE);  // Or USB, SIMULATED
hid_manager_enable();

// Send keyboard input
hid_keyboard_key_press(HID_KEY_A);
hid_keyboard_key_release(HID_KEY_A);

// Send modifier + key combo
hid_keyboard_modifier_press(HID_MOD_CTRL);
hid_keyboard_key_press(HID_KEY_C);
hid_keyboard_key_release(HID_KEY_C);
hid_keyboard_modifier_release(HID_MOD_CTRL);

// Send gamepad input
hid_gamepad_button_press(HID_GAMEPAD_BUTTON_A);
hid_gamepad_axis_move(HID_GAMEPAD_AXIS_LEFT_X, 127);  // -128 to 127
hid_gamepad_button_release(HID_GAMEPAD_BUTTON_A);
```

### Kconfig Options

```kconfig
CONFIG_AKIRA_HID=y           # Enable HID subsystem
CONFIG_AKIRA_HID_KEYBOARD=y  # Keyboard profile
CONFIG_AKIRA_HID_GAMEPAD=y   # Gamepad profile
CONFIG_AKIRA_BT_HID=y        # Enable Bluetooth HID transport
CONFIG_AKIRA_USB_HID=y       # Enable USB HID transport
CONFIG_AKIRA_HID_SIM=y       # Enable simulation (native_sim only)
```

## HTTP Server

### Features

- **Route Registration**: Define handlers for paths and methods
- **WebSocket Support**: Real-time bidirectional communication
- **Upload Streaming**: Handle large file uploads in chunks
- **Statistics**: Track requests, connections, bytes transferred

### Usage

```c
#include "http_server.h"

// Initialize and start
akira_http_server_init();
akira_http_server_start();

// Register route
int handle_status(http_request_t *req, http_response_t *resp) {
    resp->status_code = 200;
    resp->content_type = HTTP_CONTENT_JSON;
    resp->body = "{\"status\":\"ok\"}";
    resp->body_len = 15;
    return 0;
}

http_route_t route = {
    .method = HTTP_GET,
    .path = "/api/status",
    .handler = handle_status
};
akira_http_register_route(&route);

// Enable WebSocket
akira_http_enable_websocket();
akira_http_ws_register_message_cb(on_message, NULL);

// Send to all WebSocket clients
akira_http_ws_send_text(-1, "{\"event\":\"update\"}");
```

### Kconfig Options

```kconfig
CONFIG_AKIRA_HTTP_SERVER=y       # Enable HTTP server
CONFIG_AKIRA_HTTP_WEBSOCKET=y    # Enable WebSocket support
CONFIG_AKIRA_HTTP_PORT=80        # Server port
CONFIG_AKIRA_HTTP_MAX_CONNECTIONS=4  # Max concurrent connections
```

## OTA Transport System

### Architecture

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

### Usage

```c
#include "ota_transport.h"

// Initialize transport system
ota_transport_init();

// Enable specific sources
ota_transport_set_enabled_sources(OTA_SOURCE_HTTP | OTA_SOURCE_BLE);

// Register callbacks
ota_transport_set_data_cb(on_ota_data);
ota_transport_set_complete_cb(on_ota_complete);

// OTA data callback
int on_ota_data(const uint8_t *data, size_t len, size_t offset, size_t total) {
    // Write to flash partition
    return ota_manager_write_chunk(data, len);
}

// OTA complete callback
void on_ota_complete(bool success, const char *error) {
    if (success) {
        ota_manager_finalize();
        sys_reboot(SYS_REBOOT_COLD);
    } else {
        LOG_ERR("OTA failed: %s", error);
    }
}
```

### Transport Files

| File | Description |
|------|-------------|
| `ota_transport.h` | Transport interface definitions |
| `ota_transport.c` | Transport registry and routing |
| `transports/ota_http.c` | HTTP upload via web server |
| `transports/ota_ble.c` | BLE GATT OTA service |
| `transports/ota_usb.c` | USB CDC ACM transfer |
| `transports/ota_cloud.c` | Cloud integration (stub) |

### Kconfig Options

```kconfig
CONFIG_AKIRA_OTA=y           # Enable OTA system
CONFIG_AKIRA_OTA_HTTP=y      # HTTP transport
CONFIG_AKIRA_OTA_BLE=y       # Bluetooth transport
CONFIG_AKIRA_OTA_USB=y       # USB transport
CONFIG_AKIRA_OTA_CLOUD=n     # Cloud transport (future)
CONFIG_AKIRA_OTA_MAX_CHUNK_SIZE=4096  # Chunk size
```

## Client Connectivity

AkiraOS includes client-side connectivity for connecting **to** external servers.

### WebSocket Client

Connect to WebSocket servers for real-time communication with cloud services or control apps.

```c
#include "ws_client.h"

// Define callbacks
void on_message(const char *data, size_t len, bool binary, void *user) {
    LOG_INF("Received: %.*s", len, data);
}

void on_event(akira_ws_event_t event, void *user) {
    switch (event) {
    case AKIRA_WS_EVENT_CONNECTED:
        LOG_INF("WebSocket connected!");
        break;
    case AKIRA_WS_EVENT_DISCONNECTED:
        LOG_WRN("WebSocket disconnected");
        break;
    }
}

// Initialize
akira_ws_init();

// Configure callbacks
akira_ws_callbacks_t cbs = {
    .on_message = on_message,
    .on_event = on_event,
    .user_data = NULL
};

// Connect to server
akira_ws_connect("ws://192.168.1.100:8080/api", &cbs);

// Send messages
akira_ws_send_text("Hello server!");
akira_ws_send_binary(data, len);

// Later: disconnect
akira_ws_disconnect();
```

#### Auto-Reconnect

```c
akira_ws_reconnect_config_t rc = {
    .enabled = true,
    .initial_delay_ms = 1000,
    .max_delay_ms = 30000,
    .max_attempts = 0  // Infinite
};
akira_ws_set_reconnect(&rc);
```

### CoAP Client

Connect to CoAP servers for IoT cloud platforms (LwM2M, Eclipse IoT, etc.).

```c
#include "coap_client.h"

// Initialize
coap_client_init();

// Simple GET request
coap_response_t resp;
int ret = coap_client_get("coap://192.168.1.50:5683/sensors/temp", &resp);
if (ret == 0 && resp.code == COAP_CODE_CONTENT) {
    LOG_INF("Temperature: %.*s", resp.payload_len, resp.payload);
}
coap_client_free_response(&resp);

// POST with JSON
const char *json = "{\"value\":25.5}";
coap_client_post("coap://server/data", 
                 (uint8_t *)json, strlen(json),
                 COAP_FORMAT_JSON, &resp);
coap_client_free_response(&resp);

// PUT for updates
coap_client_put("coap://server/config",
                config_data, config_len,
                COAP_FORMAT_CBOR, &resp);

// DELETE resource
coap_client_delete("coap://server/old-resource", &resp);

// Download large file (block transfer)
uint8_t buffer[8192];
size_t received;
coap_client_download("coap://server/firmware.bin", 
                     buffer, sizeof(buffer), &received);
```

#### Observe (Push Notifications)

```c
void on_sensor_update(const coap_response_t *resp, void *user) {
    LOG_INF("Sensor update: %.*s", resp->payload_len, resp->payload);
}

// Start observing
coap_observe_handle_t h = coap_client_observe(
    "coap://server/sensors/motion",
    on_sensor_update, NULL);

// Later: stop observing
coap_client_observe_stop(h);
```

#### DTLS Security (CoAPS)

```c
// Set pre-shared key
const uint8_t psk[] = {0x01, 0x02, 0x03, ...};
coap_client_set_psk(psk, sizeof(psk), "device-001");

// Now use coaps:// URLs
coap_client_get("coaps://secure-server:5684/data", &resp);
```

### Kconfig Options

```kconfig
# WebSocket Client
CONFIG_AKIRA_WS_CLIENT=y          # Enable WebSocket client
CONFIG_AKIRA_WS_AUTO_RECONNECT=y  # Auto-reconnect
CONFIG_AKIRA_WS_RECONNECT_DELAY_MS=1000  # Initial delay

# CoAP Client
CONFIG_AKIRA_COAP_CLIENT=y        # Enable CoAP client
CONFIG_AKIRA_COAP_DTLS=y          # DTLS security (coaps://)
CONFIG_AKIRA_COAP_OBSERVE=y       # Observe support
```

## Bluetooth Manager

### Features

- **Stack Management**: Initialize, enable, disable BT stack
- **Advertising**: Configurable advertising modes
- **Connections**: Track connected devices, pairing
- **HID Service**: Integrated BLE HID service

### Usage

```c
#include "bt_manager.h"

// Initialize Bluetooth
akira_bt_init();

// Start advertising as HID device
akira_bt_advertise_start(AKIRA_BT_ADV_HID);

// Register callbacks
akira_bt_register_connect_cb(on_connect);
akira_bt_register_disconnect_cb(on_disconnect);

// Check connection
if (akira_bt_is_connected()) {
    // Send HID report
}

// Stop advertising
akira_bt_advertise_stop();
```

## USB Manager

### Features

- **Device Stack**: USB device enumeration
- **Class Control**: Enable/disable device classes
- **HID Class**: USB keyboard/gamepad

### Usage

```c
#include "usb_manager.h"

// Initialize USB
akira_usb_init();

// Enable HID class
akira_usb_enable_class(AKIRA_USB_CLASS_HID);

// Check enumeration
if (akira_usb_is_enumerated()) {
    // Device is connected to host
}
```

## Platform HAL vs System HAL

AkiraOS uses a two-layer HAL architecture:

### Platform HAL (`src/drivers/platform_hal.h`)

Hardware-specific operations:
- GPIO control (pins, buttons)
- SPI bus access
- Display simulation for native_sim

### System HAL (`src/akira/hal/hal.h`)

System-level operations:
- Version information
- Uptime tracking
- Device identification
- Reboot/shutdown control

This separation allows platform-specific code to be isolated from system-level abstractions.
