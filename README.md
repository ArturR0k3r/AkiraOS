# AkiraOS now uses OCRE as its container and WASM runtime

**Zephyr Version:** v4.3.0 (upstream stable)  
**Build Status:** ✅ Native Simulator | ⚠️ ESP32 (Known Issue)

> **Quick Start:** `./build_and_run.sh` - See [docs/QUICKSTART.md](docs/QUICKSTART.md)  
> **Build Status:** See [docs/BUILD_STATUS.md](docs/BUILD_STATUS.md) for platform details

AkiraOS integrates [OCRE](https://github.com/project-ocre/ocre-runtime) for secure, lightweight container and WebAssembly app management on Zephyr and ESP32 platforms. All app lifecycle operations (upload, start, stop, list) are handled via OCRE APIs.

## Platform Support

| Platform | Status | Build Command | Notes |
|----------|--------|---------------|-------|
| **Native Simulator** | ✅ Working | `./build_and_run.sh` | Recommended for development |
| **ESP32-S3** | ⚠️ Blocked | N/A | Zephyr 4.3.0 POSIX timer issue |
| **ESP32** | ⚠️ Blocked | N/A | Zephyr 4.3.0 POSIX timer issue |
| **ESP32-C3** | ⚠️ Blocked | N/A | Zephyr 4.3.0 POSIX timer issue |

See [docs/BUILD_STATUS.md](docs/BUILD_STATUS.md) for details and workarounds.

## Build Instructions (OCRE Integration)

- OCRE is included as a west module (see `west.yml`).
- The build system automatically links OCRE sources and headers.
- No stub logic: all app/container management is delegated to OCRE.

### Example Usage

```c
// Upload a WASM app or container
ocre_runtime_load_app("my_app", binary, size);
// Start the app
ocre_runtime_start_app("my_app");
// Stop the app
ocre_runtime_stop_app("my_app");
// List all containers/apps
ocre_runtime_list_apps(app_list, max_count);
```

## Features
- Secure container runtime for embedded devices
- WebAssembly app support
- Unified API for upload, start, stop, and list operations
- Zephyr and ESP32-S3/ESP32 support

## For full technical details, see docs/api-reference.md and OCRE documentation.

---

## Core Features & Implementation

### Graphics System
- Pixel-perfect renderer, tile engine, sprite system
- CRT effects (scanlines, bloom, curvature)
- UI framework for cyberpunk widgets
- Double buffering and DMA transfer for display

### Input System
- Debounced button input, event queue
- Combo detection, long/short press, rapid-fire
- Input recording/playback for demos

### Security Toolkit
- Wi-Fi scanner, deauth detector, packet sniffer, ARP scanner, network mapper
- BLE tools, password generator, hash calculator, QR code utilities
- Ethical use notice: for education and authorized testing only

### WASM/OCRE Integration
- WAMR executes sandboxed WASM modules (games, tools)
- OCRE provides container isolation and resource limits
- Host APIs for graphics, input, audio, network, security

See `src/akiraos.c` and `docs/api-reference.md` for integration details.
# AkiraOS: Comprehensive Architecture & Strategic Positioning

## Executive Summary

AkiraOS is a minimalist retro-cyberpunk gaming console and hacker toolkit powered by ESP32-S3 and WebAssembly. It uniquely positions itself as a dual-purpose embedded platform: a nostalgic gaming device AND a portable cybersecurity toolkit, running on Zephyr RTOS with WASM runtime support.

---

## System Logic Overview

AkiraOS is built from modular subsystems:
- **System Services**: Managed by `service_manager` (graphics, input, network, storage, audio, security, UI)
- **Event System**: Central event bus for inter-module communication
- **Process Management**: Launches and tracks native/WASM apps
- **WASM Runtime (WAMR)**: Executes sandboxed WASM modules
- **OCRE Runtime**: Container isolation for apps
- **Drivers**: Display, buttons, storage, audio, etc.
- **OTA/Bluetooth/Shell/Settings**: Modular, extensible system services

### Main System Logic
See `src/akiraos.c` for initialization and orchestration of all subsystems.

### Extensibility
The architecture supports future features (audio, SD card, multiplayer, advanced graphics, security tools, etc.) via modular service registration and event-driven design.

---

## For full technical and strategic details, see `docs/api-reference.md` and the architecture section below.

# AkiraOS

```
 █████╗ ██╗  ██╗██╗██████╗  █████╗        ██████╗ ███████╗  
██╔══██╗██║ ██╔╝██║██╔══██╗██╔══██╗      ██╔═══██╗██╔════╝  
███████║█████╔╝ ██║██████╔╝███████║█████╗██║   ██║███████╗  
██╔══██║██╔═██╗ ██║██╔══██╗██╔══██║╚════╝██║   ██║╚════██║  
██║  ██║██║  ██╗██║██║  ██║██║  ██║      ╚██████╔╝███████║  
╚═╝  ╚═╝╚═╝  ╚═╝╚═╝╚═╝  ╚═╝╚═╝  ╚═╝       ╚═════╝ ╚══════╝  
```

**A minimalist retro-cyberpunk gaming console and hacker toolkit powered by ESP32 and WebAssembly**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![Zephyr RTOS](https://img.shields.io/badge/RTOS-Zephyr-blue)](https://zephyrproject.org/)
[![WebAssembly](https://img.shields.io/badge/Runtime-WASM-purple)](https://webassembly.org/)

---

## 🚀 Quick Start

**New to AkiraOS?** Check out the **[Quick Start Guide](docs/QUICK_START.md)** to get running in 5 minutes!

```bash
# Clone, build, and flash ESP32-S3 Console
cd ~ && mkdir Akira && cd Akira
git clone <your-repo> AkiraOS && cd AkiraOS
west init -l . && cd .. && west update
cd AkiraOS && ./build_both.sh esp32s3
./flash.sh
west espmonitor
```

**📚 Documentation:**
- **[QUICK_START.md](docs/QUICK_START.md)** - Get started in 5 minutes
- **[BUILD_SCRIPTS.md](docs/BUILD_SCRIPTS.md)** - Complete build guide
- **[BUILD_PLATFORMS.md](docs/BUILD_PLATFORMS.md)** - Platform comparison
- **[SENSOR_INTEGRATION.md](docs/SENSOR_INTEGRATION.md)** - Sensor modules

---

## 🎮 Overview

AkiraOS is an open-source gaming console that combines the power of modern embedded systems with the nostalgic appeal of retro gaming. Built on the ESP32-S3 microcontroller and running a custom Zephyr RTOS, it supports WebAssembly applications and doubles as a cybersecurity toolkit for ethical hacking and network analysis.
![DSC_0078](https://github.com/user-attachments/assets/631810c2-bb2a-4731-b8fc-b11a841c5733)


### Key Features

- 🎯 **Retro Gaming**: Run classic-style games compiled to WebAssembly
- 🔧 **Hacker Toolkit**: Built-in cybersecurity tools and CLI access
- 🌐 **Network Capable**: Wi-Fi and Bluetooth connectivity
- 🔋 **Portable**: Battery-powered with USB-C charging
- 🎨 **Customizable**: Cyberpunk-themed UI with multiple skins
- 📱 **Modern Architecture**: WebAssembly runtime on embedded hardware

## 🛠️ Technical Specifications

### Supported Platforms

AkiraOS runs on **multiple platforms** with a unified codebase:

| Platform | Status | Use Case |
|----------|--------|----------|
| **ESP32-S3** | ✅ Production | **Akira Console** - Full hardware support (display, WiFi, OTA) |
| **ESP32** | ✅ Production | **Akira Console** (Legacy) - Full hardware support |
| **ESP32-C3** | ✅ Production | **Akira Modules Only** - Remote sensors/peripherals |
| **native_sim** | ✅ Development | x86 simulation for testing |

> **Important:** ESP32-C3 is designed exclusively for **Akira Modules** (remote sensor nodes, wireless peripherals, distributed control). It is **NOT** suitable for the Akira Console handheld device. See [docs/BUILD_PLATFORMS.md](docs/BUILD_PLATFORMS.md) for details.

### Hardware (Production Boards)

| Component | Specification |
|-----------|---------------|
| **Microcontroller** | ESP32-S3-WROOM-32 / ESP32-WROOM-32 |
| **Connectivity** | Wi-Fi 802.11 b/g/n, Bluetooth 5.0 |
| **Display** | 2.4" TFT SPI (ILI9341) - 240×320 resolution |
| **Power** | Li-ion battery with USB-C TP4056 charging |
| **Controls** | D-Pad + 4 action buttons |
| **Memory** | 512KB SRAM, 8MB PSRAM, 16MB Flash (ESP32-S3) |

### Software Architecture

- **Operating System**: Custom Zephyr RTOS
- **Runtime**: WAMR (WebAssembly Micro Runtime)
- **Container Support**: OCRE (Open Containers Runtime Environment)
- **Development Languages**: C/C++/Rust → WebAssembly
- **Graphics**: Custom pixel-art renderer with CRT effects

![DSC_0081](https://github.com/user-attachments/assets/8a6ec23f-e7b3-4180-b24c-6537f7b01069)

## 🚀 Quick Start

### Prerequisites

- **Zephyr SDK**: Follow the [official installation guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
- **Python 3.8+**: For build tools
- **West Tool**: Zephyr's meta-tool for project management
- **ESP-IDF Tools**: For ESP32 development

> **Note**: Development is recommended on WSL (Windows Subsystem for Linux) or native Linux/macOS.

### Installation

1. **Clone the repository**
   ```bash
   mkdir AkiraOS-workspace
   cd AkiraOS-workspace
   git clone https://github.com/ArturR0k3r/AkiraOS.git
   cd AkiraOS/
   ```

2. **Initialize West workspace**
   ```bash
   west init -l .
   west update
   west blobs fetch hal_espressif
   ```

3. **Open development environment**
   ```bash
   code AkiraOS.code-workspace
   ```

## 🔨 Building

**📚 For detailed build instructions, see [BUILD_SCRIPTS.md](docs/BUILD_SCRIPTS.md)**

### Quick Start - Build All Platforms

```bash
# Build for all platforms at once
./build_all.sh

# Or build specific platform
./build_all.sh esp32s3    # ESP32-S3 Console (Primary)
./build_all.sh esp32c3    # ESP32-C3 Modules Only
./build_all.sh native_sim # Native simulation
```

This will build:
- ✅ native_sim (simulation/testing)
- ✅ ESP32-S3 (Akira Console - Primary)
- ✅ ESP32 (Akira Console - Legacy)
- ✅ ESP32-C3 (Akira Modules Only)

### Platform-Specific Builds with MCUboot

```bash
# Build MCUboot + AkiraOS for specific platform
./build_both.sh esp32s3       # ESP32-S3 Console
./build_both.sh esp32c3       # ESP32-C3 Modules
./build_both.sh esp32s3 clean # Clean and build

# Flash to device

# Flash to device
./flash.sh                    # Auto-detect platform
./flash.sh --platform esp32s3 # Specify platform
```

### Manual Platform Builds

#### Native Simulation (Testing)
```bash
cd /path/to/Akira
west build --pristine -b native_sim AkiraOS -d build_native_sim

# Run simulation
./build_native_sim/zephyr/zephyr.exe
```

#### ESP32-S3 DevKitM (Akira Console - Primary)
```bash
cd /path/to/Akira
west build --pristine -b esp32s3_devkitm/esp32s3/procpu AkiraOS -d build_esp32s3
west flash -d build_esp32s3
```

#### ESP32 DevKitC (Legacy Console)
```bash
cd /path/to/Akira
west build --pristine -b esp32_devkitc/esp32/procpu AkiraOS -d build_esp32
west flash -d build_esp32
```

#### ESP32-C3 DevKitM (Akira Modules Only)
```bash
cd /path/to/Akira
west build --pristine -b esp32c3_devkitm AkiraOS -d build_esp32c3
west flash -d build_esp32c3
```

**⚠️ Important:** ESP32-C3 is for Akira Modules only, not for Akira Console! See [BUILD_PLATFORMS.md](docs/BUILD_PLATFORMS.md) for details.

### VS Code Integration

Press `Ctrl+Shift+B` to run the configured build task.

## 📱 Flashing Firmware

**📚 For detailed flashing instructions, see [BUILD_SCRIPTS.md](docs/BUILD_SCRIPTS.md)**

### Quick Flash (Auto-Detection)

```bash
# Auto-detect chip and flash everything
./flash.sh

# Flash to specific platform
./flash.sh --platform esp32s3
./flash.sh --platform esp32c3

# Flash only application (faster updates)
./flash.sh --app-only

# Flash to specific port
./flash.sh --port /dev/ttyUSB0
```

### Manual Flashing with esptool

```bash
# ESP32-S3 Console
esptool --chip esp32s3 write-flash 0x1000 build-mcuboot/zephyr/zephyr.bin
esptool --chip esp32s3 write-flash 0x20000 build/zephyr/zephyr.signed.bin

# ESP32-C3 Modules
esptool --chip esp32c3 write-flash 0x1000 build-mcuboot/zephyr/zephyr.bin
esptool --chip esp32c3 write-flash 0x20000 build/zephyr/zephyr.signed.bin

# ESP32 Legacy Console
esptool --chip esp32 write-flash 0x1000 build-mcuboot/zephyr/zephyr.bin
esptool --chip esp32 write-flash 0x20000 build/zephyr/zephyr.signed.bin
```

### Monitoring Serial Output

```bash
# Using west espmonitor (recommended)
west espmonitor --port /dev/ttyUSB0

# Using screen
screen /dev/ttyUSB0 115200

# Using picocom
picocom -b 115200 /dev/ttyUSB0
```
````
```

### Manual Platform Builds

#### Native Simulation (Testing)
```bash
cd /path/to/Akira
west build --pristine -b native_sim AkiraOS -d build_native_sim

# Run simulation
./build_native_sim/zephyr/zephyr.exe
```

#### ESP32-S3 DevKitM (Akira Console - Primary)
```bash
cd /path/to/Akira
west build --pristine -b esp32s3_devkitm/esp32s3/procpu AkiraOS -d build_esp32s3
west flash -d build_esp32s3
```

#### ESP32 DevKitC (Original)
```bash
cd /path/to/Akira
west build --pristine -b esp32_devkitc/esp32/procpu AkiraOS -d build_esp32
west flash -d build_esp32
```

### VS Code Integration

Press `Ctrl+Shift+B` to run the configured build task.

## 📱 Flashing Firmware

### Manual Flashing

```bash
# Flash MCUboot bootloader
esptool write-flash 0x1000 build-mcuboot/zephyr/zephyr.bin

# Flash AkiraOS application
esptool write-flash 0x20000 build/zephyr/zephyr.signed.bin
```

### Automated Flashing

```bash
chmod +x flash.sh

# Flash both bootloader and application
./flash.sh

# Flash only bootloader
./flash.sh --bootloader-only

# Flash only application  
./flash.sh --app-only
```

## 🎮 Usage

### Gaming Mode

1. Power on the console
2. Use the D-pad to navigate the menu
3. Select games from the installed WASM applications
4. Use action buttons for gameplay

### Hacker Mode

> **Coming Soon**: Terminal interface with network analysis tools, Wi-Fi scanning, and cybersecurity utilities.

## 🧩 Development

### Creating WASM Applications

For detailed information on developing WASM applications for AkiraOS, please refer to our comprehensive [API Documentation](docs/api-reference.md) and [Game Development Tutorial](docs/game-development.md).

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](CONTRIBUTING.md) for details.

### Development Workflow

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Make your changes
4. Test on hardware if possible
5. Commit your changes (`git commit -m 'Add amazing feature'`)
6. Push to the branch (`git push origin feature/amazing-feature`)
7. Open a Pull Request

### Code Style

- Follow Zephyr coding standards
- Use clear, descriptive variable names
- Comment complex logic
- Include unit tests where applicable

## 📚 Documentation

- [Hardware Assembly Guide](docs/hardware-assembly.md)
- [API Documentation](docs/api-reference.md)
- [Troubleshooting](docs/troubleshooting.md)

## 🛒 Hardware Availability

**Hardware kits will be available for purchase later.** Stay tuned for updates on availability and pricing.


## 🙏 Acknowledgments

- **[Zephyr Project](https://zephyrproject.org/)** - Powerful embedded RTOS
- **[OCRE Project](https://opencontainers.org/)** - Open container runtime environment
- **[WebAssembly](https://webassembly.org/)** - Platform-agnostic runtime
- **[WAMR](https://github.com/bytecodealliance/wasm-micro-runtime)** - WebAssembly micro runtime

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 📞 Support

- **Issues**: [GitHub Issues](https://github.com/ArturR0k3r/AkiraOS/issues)
- **Discussions**: [GitHub Discussions](https://github.com/ArturR0k3r/AkiraOS/discussions)
- **Email**: support@pen.engineering

---

<div align="center">

**Made with ❤️ by the AkiraOS community**

[⭐ Star this repo](https://github.com/ArturR0k3r/AkiraOS) | [🐛 Report Bug](https://github.com/ArturR0k3r/AkiraOS/issues) | [💡 Request Feature](https://github.com/ArturR0k3r/AkiraOS/issues)

</div>
