# AkiraOS Bluetooth/BLE Module

## Purpose
This module enables AkiraOS to connect to a phone via Bluetooth/BLE for:
- Downloading applications and software updates (OTA)
- Accessing and controlling the Akira shell from a phone

## Features
- BLE advertising for phone discovery
- Secure connection management
- OTA update transfer via BLE
- Shell command and output exchange with phone
- Extensible for future Bluetooth services

## API
See `bluetooth_manager.h` for available functions:
- `bluetooth_manager_init()`
- `bluetooth_manager_start_advertising()` / `stop_advertising()`
- `bluetooth_manager_on_connect()` / `on_disconnect()`
- `bluetooth_manager_send_shell_output()` / `receive_shell_command()`
- `bluetooth_manager_send_ota_update()`

## Integration
- Register Bluetooth transport with OTA Manager for updates
- Add shell command handlers to interact with Bluetooth
- Use shell over BLE for remote diagnostics and control

## Example Usage
```c
bluetooth_manager_init();
bluetooth_manager_start_advertising();
// On phone connect:
bluetooth_manager_on_connect();
// Send shell output:
bluetooth_manager_send_shell_output("Akira> help\n");
// Receive shell command from phone:
bluetooth_manager_receive_shell_command("ota status");
```

## Next Steps
- Implement BLE GATT services for OTA and shell
- Add shell command integration (see shell/akira_shell.c)
- Test phone connectivity and update transfer
