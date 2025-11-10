# AkiraOS Source Directory Structure

This directory contains the **core AkiraOS functionality**. Third-party modules and dependencies are located in the separate `modules/` directory.

## Directory Organization

### Core Components (`src/`)

- **`akira_modules/`** - Akira Module System for external project integration
  - Module registration and management
  - Communication interfaces (UART, SPI, I2C, Network)
  - Hardware control APIs (Display, Buttons, GPIO)
  - Event broadcasting system
  - Protocol handlers (JSON, MessagePack)

- **`drivers/`** - Hardware drivers
  - `display_ili9341.c` - ILI9341 TFT display driver
  - `fonts.c` - Font rendering
  - `akira_hal.c` - Hardware abstraction layer
  - `sim/` - Simulation drivers for native_sim

- **`shell/`** - Interactive shell
  - `akira_shell.c` - Command-line interface
  - System diagnostics and control

- **`settings/`** - Configuration management
  - `settings.c` - Persistent settings storage
  - WiFi credentials, device ID, preferences

- **`OTA/`** - Over-The-Air updates
  - `ota_manager.c` - Firmware update management
  - `web_server.c` - HTTP/WebSocket server for OTA

- **`apps/`** - Built-in applications
  - User-facing applications
  - Example programs

- **`ui/`** - User interface components
  - UI framework
  - Display management

- **`lib/`** - Utility libraries
  - Helper functions
  - Common utilities

- **`runtime/`** - Application runtime
  - Container runtime integration
  - App lifecycle management

- **`bluetooth/`** - Bluetooth support
  - BLE communication
  - Device pairing

- **`services/`** - System services
  - Background services
  - System daemons

### Third-Party Modules (`../modules/`)

The `modules/` directory contains external dependencies:

- **`wasm-micro-runtime/`** - WebAssembly runtime (WAMR)
- **`ocre/`** - OCRE container runtime
- Other third-party libraries

## Design Philosophy

### Why This Structure?

**Core vs Third-Party Separation:**
- `src/` contains code we write and maintain
- `modules/` contains upstream third-party code
- Clear ownership and maintenance boundaries
- Easy to update third-party dependencies without affecting core code

**Benefits:**
1. **Clear Organization** - Easy to find what you need
2. **Module Independence** - Each directory is self-contained
3. **Build System** - CMake naturally handles this structure
4. **Git Management** - Separate .gitignore rules for third-party code
5. **Documentation** - README in each major component

## Adding New Components

### Adding Core AkiraOS Functionality
→ Add to `src/` directory with appropriate subdirectory

### Adding Third-Party Library
→ Add to `modules/` directory and update `modules/modules.cmake`

## Example: Akira Module System

The `akira_modules/` system is **core AkiraOS functionality**, not a third-party module:

```
src/akira_modules/          ← Core AkiraOS component
├── include/
│   └── akira_module.h      ← Public API
├── src/
│   ├── akira_module_core.c
│   ├── akira_module_manager.c
│   └── akira_module_registry.c
└── examples/
    ├── display_module.c
    ├── button_module.c
    └── gpio_module.c

modules/                     ← Third-party dependencies
├── wasm-micro-runtime/     ← External project
├── ocre/                   ← External project
└── modules.cmake
```

## Building

The build system automatically includes all core components:

```bash
# All src/ components are built automatically
west build -b esp32s3_devkitm/esp32s3/procpu

# Enable akira_modules in prj.conf:
CONFIG_AKIRA_MODULE=y
```

## Learn More

- **Akira Modules:** See `akira_modules/README.md`
- **Drivers:** See `drivers/README.md` (if exists)
- **Shell:** See `shell/README.md` (if exists)
- **Main Documentation:** See `../docs/`

---

**Remember:** `src/` = Our Code | `modules/` = Their Code
