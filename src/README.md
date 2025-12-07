# AkiraOS Source Directory Structure

This directory contains the **core AkiraOS v2.0 functionality**.

## Directory Organization

### Core Kernel (`src/akira/`)

The heart of AkiraOS - kernel services and hardware abstraction:

- **`akira.h`** - Central include file for all AkiraOS APIs
- **`init.c`** - System initialization sequence
- **`shell.c`** - Debug shell commands (`akira status`, `akira memory`, etc.)
- **`kernel/`** - Kernel subsystems:
  - `types.h` - Core types (handles, results, priorities)
  - `service.h/c` - Service manager with dependencies
  - `event.h/c` - Event system with async dispatch
  - `process.h/c` - Process lifecycle management
  - `memory.h/c` - Memory pool management
  - `timer.h/c` - Software timers and time utilities
- **`hal/`** - Hardware abstraction layer:
  - `hal.h/c` - GPIO, SPI, I2C abstraction

### API Layer (`src/api/`)

WASM-exported APIs for applications:

- `akira_display_api.c` - Display/graphics functions
- `akira_input_api.c` - Button/touch input
- `akira_rf_api.c` - RF communication
- `akira_sensor_api.c` - Sensor access
- `akira_storage_api.c` - File/flash storage
- `akira_network_api.c` - Network operations
- `akira_system_api.c` - System info/control

### Security (`src/security/`)

Trust and capability system:

- `trust_levels.h` - Trust level definitions (Kernel/System/Trusted/User)
- `capability.h/c` - Capability-based permissions
- `app_signing.h/c` - App signature verification

### Power Management (`src/power/`)

- `power_manager.h/c` - Power modes, sleep, battery monitoring

### IPC (`src/ipc/`)

Inter-process communication:

- `message_bus.h/c` - Pub/sub message passing
- `shared_memory.h/c` - Shared memory regions

### Resource Management (`src/resource/`)

- `resource_manager.h/c` - Resource allocation/tracking
- `scheduler.h/c` - Process scheduling

### UI Framework (`src/ui/`)

- `ui_framework.h/c` - Widget system, event handling

### App Loader (`src/apps/`)

- `app_loader.h/c` - WASM app loading and verification

### Drivers (`src/drivers/`)

Hardware drivers:

- `display_ili9341.c` - ILI9341 TFT display
- `ssd1306.c` - SSD1306 OLED (optional)
- `st7789.c` - ST7789 TFT (optional)
- `rf_framework.c` - Unified RF driver framework
- `lr1121.c`, `cc1101.c` - RF chip drivers
- `bme280.c`, `lsm6ds3.c`, `ina219.c` - Sensors
- `nrf24l01.c` - 2.4GHz transceiver
- `akira_hal.c` - Legacy HAL (use akira/hal/ instead)
- `fonts.c`, `font_data.c` - Font rendering

### Services (`src/services/`)

Runtime services:

- `ocre_runtime.h/c` - OCRE container runtime wrapper
- `wasm_app_manager.h/c` - WASM app lifecycle

### Other Components

- **`bluetooth/`** - Bluetooth manager
- **`shell/`** - Legacy shell commands
- **`settings/`** - Persistent settings
- **`OTA/`** - Over-the-air updates, web server
- **`lib/`** - Utility libraries (POSIX compat)
- **`akira_modules/`** - External module integration

## Usage

Include the main header to access all AkiraOS APIs:

```c
#include "akira/akira.h"

int main(void) {
    akira_init();
    akira_start();
    // ...
}
```

## Build

```bash
west build -b esp32s3_devkitm/esp32s3/procpu
```
