# AkiraOS OTA Manager & Modular Architecture

## Modular OTA Architecture

```
+-------------------+      +-------------------+      +-------------------+
- **WebServer**: HTTP-based OTA (temporary, for local network updates).
- **BLE**: Future support for mobile app-based updates.
- **USB**: Future support for PC app-based updates.
- **Cloud**: Future support for AkiraHub/cloud-based updates.
- Each transport implements a standard interface to interact with OTA Manager.
+-------------------+
```

### Usage
1. OTA Manager is initialized at startup.
2. Transport modules (WebServer, Bluetooth) register themselves via `ota_manager_register_transport()`.
3. OTA Manager receives firmware data via registered transports.
4. OTA Manager validates, installs, and manages rollback.

### Extending Transports
To add a new transport (e.g., USB, Cloud):
1. Implement the `ota_transport_t` interface in your module.
2. Register with `ota_manager_register_transport()`.
3. Send OTA data chunks and report progress via the transport API.


### Example Transport Registration
```c
// In your transport module:
static ota_transport_t my_transport = {
        .name = "my_transport",
        .start = my_start_fn,
        .stop = my_stop_fn,
        .send_chunk = my_send_chunk_fn,
        .report_progress = my_report_progress_fn,
        .user_data = NULL
};
ota_manager_register_transport(&my_transport);
```

---

## Button Driver (Direct GPIO)

AkiraOS now includes a direct GPIO button driver for fast, reliable access to all gaming buttons:

- Location: `src/drivers/akira_buttons.c` / `akira_buttons.h`
- API:
        - `int akira_buttons_init(void);` // Initialize GPIO pins for buttons
        - `uint16_t akira_buttons_get_state(void);` // Get current button state (bitmask)
- Button bitmasks match the `enum gaming_button` in shell and HAL modules.

This enables low-latency button reads for gaming and system control, and is compatible with all supported boards.

### Settings Module
- Manages persistent configuration and device settings.
- Exposes API for reading/writing settings.
- Independent from OTA and transport logic.

### Shell Module
- Provides command-line interface for diagnostics and control.
- Can trigger OTA, settings changes, etc.
- Modular and reusable in other contexts.

## Extending OTA
- To add a new transport, implement the transport interface and register with OTA Manager.
- OTA Manager does not need to change for new transports.
- Example: Add BLE transport for mobile app updates.

## Example Usage

```c
// Initialize OTA Manager
ota_manager_init();

// Register transports
ota_manager_register_transport(&web_server_transport);
ota_manager_register_transport(&ble_transport);
ota_manager_register_transport(&usb_transport);

// Trigger OTA update (from any transport)
ota_manager_start_update(update_source);
```

## Benefits
- **Decoupling**: OTA logic is independent of how updates arrive.
- **Reusability**: Settings and Shell can be used in other projects.
- **Extensibility**: New update sources can be added with minimal changes.
- **Maintainability**: Clear separation of concerns.

## Status
- OTA Manager and WebServer are now separate modules.
- Settings and Shell are modularized.
- BLE, USB, and Cloud transports are planned for future releases.

---

For implementation details, see:
- `ota_manager.c` / `ota_manager.h`
- `web_server.c` / `web_server.h`
- `settings.c` / `settings.h`
- `shell/akira_shell.c` / `shell/akira_shell.h`
