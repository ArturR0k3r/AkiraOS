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

> **📖 New to AkiraOS?** See **[QUICKSTART.md](QUICKSTART.md)** for detailed setup instructions!

### First Time Setup

```bash
# 1. Clone repository
git clone https://github.com/ArturR0k3r/AkiraOS.git
cd AkiraOS

# 2. Initialize West workspace
west init -l .
cd ..
west update

# 3. Clone WASM-Micro-Runtime (not tracked in repo)
cd AkiraOS/modules
git clone https://github.com/bytecodealliance/wasm-micro-runtime.git
cd wasm-micro-runtime && git submodule update --init --recursive && cd ../..

# 4. Update OCRE submodules
cd ../ocre && git submodule update --init --recursive && cd ../AkiraOS

# 5. Fetch ESP32 binary blobs
cd ..
west blobs fetch hal_espressif

# 6. Build and flash
cd AkiraOS
./build_both.sh esp32s3      # Build with MCUboot
./flash.sh                   # Flash to ESP32-S3
west espmonitor              # Monitor output
```

### Quick Build (After Setup)

```bash
cd ~/Akira/AkiraOS

# Build and flash ESP32-S3
./build_both.sh esp32s3
./flash.sh

# Or run native simulation (no hardware needed)
./build_and_run.sh
```

### OCRE & WASM Integration

AkiraOS integrates **OCRE** (Open Container Runtime Environment) and **WASM-Micro-Runtime** as Zephyr modules:

**Automatic Integration:**
- OCRE runtime is fetched via `west.yml` from [project-ocre/ocre-runtime](https://github.com/project-ocre/ocre-runtime)
- WASM-Micro-Runtime is included in `modules/wasm-micro-runtime/`
- Both are automatically integrated through `modules/modules.cmake`
- OCRE sources are compiled via `modules/ocre/ocre.cmake`
- Build system automatically includes OCRE and WAMR headers and sources

**Usage in Code:**
```c
// OCRE Runtime API (see src/services/ocre_runtime.c)
ocre_runtime_init();                              // Initialize runtime
ocre_runtime_load_app("app", binary, size);       // Create container
ocre_runtime_start_app("app");                    // Run container
ocre_runtime_stop_app("app");                     // Stop container
ocre_runtime_destroy_app("app");                  // Cleanup

// WASM is executed through OCRE container runtime
```

**📚 Documentation:**
- **[api-reference.md](docs/api-reference.md)** - System architecture and APIs
- **[AkiraOS.md](docs/AkiraOS.md)** - Detailed architecture

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

| Platform | Status | Use Case |
|----------|--------|----------|
| **ESP32-S3** | ✅ Primary | Full Akira Console with display, WiFi, sensors |
| **ESP32** | ✅ Legacy | Akira Console (limited RAM) |
| **ESP32-C3** | ✅ Modules | Remote sensor nodes only |
| **native_sim** | ✅ Dev | Testing and simulation |

### Hardware

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

## � Building

### Build All Platforms
```bash
./build_all.sh           # Build all
./build_all.sh esp32s3   # ESP32-S3 only
./build_all.sh native_sim # Native only
```

### Build with MCUboot
```bash
./build_both.sh esp32s3        # Build bootloader + app
./build_both.sh esp32s3 clean  # Clean and build
```

### Flash to Hardware
```bash
./flash.sh                     # Auto-detect chip
./flash.sh --platform esp32s3  # Specific platform
./flash.sh --app-only          # Flash app only (faster)
```

---

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
