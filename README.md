
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

## 🎮 Overview

AkiraOS is an open-source gaming console that combines the power of modern embedded systems with the nostalgic appeal of retro gaming. Built on the ESP32-S3 microcontroller and running a custom Zephyr RTOS, it supports WebAssembly applications and doubles as a cybersecurity toolkit for ethical hacking and network analysis.

![AkiraOS Console](https://github.com/user-attachments/assets/8e9d29de-1b5c-471f-b80c-44f2f96c4fae)

### Key Features

- 🎯 **Retro Gaming**: Run classic-style games compiled to WebAssembly
- 🔧 **Hacker Toolkit**: Built-in cybersecurity tools and CLI access
- 🌐 **Network Capable**: Wi-Fi and Bluetooth connectivity
- 🔋 **Portable**: Battery-powered with USB-C charging
- 🎨 **Customizable**: Cyberpunk-themed UI with multiple skins
- 📱 **Modern Architecture**: WebAssembly runtime on embedded hardware

## 🛠️ Technical Specifications

### Hardware

| Component | Specification |
|-----------|---------------|
| **Microcontroller** | ESP32-S3-WROOM-32 |
| **Connectivity** | Wi-Fi 802.11 b/g/n, Bluetooth 5.0 |
| **Display** | 2.4" TFT SPI (ILI9341) - 240×320 resolution |
| **Power** | Li-ion battery with USB-C TP4056 charging |
| **Controls** | D-Pad + 4 action buttons |
| **Memory** | 512KB SRAM, 8MB PSRAM, 16MB Flash |

### Software Architecture

- **Operating System**: Custom Zephyr RTOS
- **Runtime**: WAMR (WebAssembly Micro Runtime)
- **Container Support**: OCRE (Open Containers Runtime Environment)
- **Development Languages**: C/C++/Rust → WebAssembly
- **Graphics**: Custom pixel-art renderer with CRT effects

![Hardware Image](https://github.com/user-attachments/assets/5d010761-cffb-4be3-8abe-2f69cc3b8900)

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

### Method 1: Command Line

```bash
# Build bootloader (MCUboot)
unset ZEPHYR_WASM_MICRO_RUNTIME_KCONFIG && \
west build --pristine -b esp32_devkitc/esp32/procpu \
    bootloader/mcuboot/boot/zephyr \
    -- -DMCUBOOT_LOG_LEVEL=4

# Build AkiraOS application  
unset ZEPHYR_BASE && \
west build --pristine -b esp32_devkitc/esp32/procpu \
    path_to_AkiraOS -d build \
    -- -DMODULE_EXT_ROOT=path_to_AkiraOS
```

### Method 2: VS Code

Press `Ctrl+Shift+B` to run the configured build task.

### Method 3: Build Script

```bash
chmod +x build_both.sh
./build_both.sh clean
```

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