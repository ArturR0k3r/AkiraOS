
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
![DSC_0078](https://github.com/user-attachments/assets/631810c2-bb2a-4731-b8fc-b11a841c5733)


### Key Features

- 🎯 **Retro Gaming**: Run classic-style games compiled to WebAssembly
- 🎵 **Piezo MEMS Audio**: Integrated micro-speaker for game sounds and notifications
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
| **ESP32-S3** | ✅ Production | Full hardware support (display, WiFi, OTA) |
| **ESP32** | ✅ Production | Full hardware support (display, WiFi, OTA) |
| **native_sim** | ✅ Development | x86 simulation for testing |

### Hardware (Production Boards)

| Component | Specification |
|-----------|---------------|
| **Microcontroller** | ESP32-S3-WROOM-32 / ESP32-WROOM-32 |
| **Connectivity** | Wi-Fi 802.11 b/g/n, Bluetooth 5.0 |
| **Display** | 2.4" TFT SPI (ILI9341) - 240×320 resolution |
| **Audio** | Piezo MEMS micro-speaker (500 Hz - 8 kHz) |
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

### Quick Start - Build All Platforms

```bash
# Build for all platforms at once
./build_all.sh
```

This will build:
- ✅ native_sim (simulation/testing)
- ✅ ESP32-S3 (full hardware)
- ✅ ESP32 (full hardware)

### Platform-Specific Builds

#### Native Simulation (Testing)
```bash
cd /path/to/Akira
west build --pristine -b native_sim AkiraOS -d build_native_sim

# Run simulation
./build_native_sim/zephyr/zephyr.exe
```

#### ESP32-S3 DevKitM
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

## 🎵 Audio System (Piezo MEMS Integration)

AkiraOS features an integrated piezoelectric MEMS micro-speaker system, developed as part of the [ST Piezo MEMS Design Challenge](https://www.st.com/content/st_com/en/events/piezo-mems-design-challenge.html).

### Audio Features

- **PWM-Driven Audio Output**: Efficient piezoelectric actuation using ESP32-S3 LEDC peripheral
- **Frequency Range**: 500 Hz - 8 kHz (optimized for retro gaming audio)
- **Sound Effects Library**: Pre-defined game sounds (coin collect, jump, explosion, power-up, etc.)
- **Low Power Consumption**: < 50 mW typical audio playback
- **WASM API**: High-level audio synthesis interface for applications
- **Shell Commands**: Interactive testing and debugging via CLI

### Audio Shell Commands

```bash
# Test audio system
akira:~$ audio test_tone

# Play specific tone (frequency, duration, volume)
akira:~$ audio tone 1000 500 80

# Frequency sweep for testing
akira:~$ audio sweep 500 5000 100

# Play sound effects
akira:~$ audio sfx coin
akira:~$ audio sfx jump
akira:~$ audio sfx explosion

# Demo all sound effects
akira:~$ audio sfx demo

# Set master volume
akira:~$ audio volume 70

# Check audio status
akira:~$ audio status
```

### Hardware Setup

For prototype testing with discrete piezo buzzer:

```
ESP32-S3 GPIO9 ──[ 100Ω ]──┬──(+) Piezo Buzzer (-)──┐
                            │                         │
                            └─────────────────────────┴── GND
```

For production with custom MEMS device, see [Piezo MEMS Design Documentation](docs/piezo-mems-speaker-design.md).

### Documentation

- **[Device Design Concept](docs/piezo-mems-speaker-design.md)** - Complete MEMS micro-speaker specifications
- **[Prototype Validation Guide](docs/piezo-prototype-validation.md)** - Testing procedures and measurement methods
- **[ST Challenge Submission Guide](docs/st-challenge-submission-guide.md)** - Competition submission preparation

### Building with Audio Support

Audio support is enabled by default. To disable:

```bash
# Disable audio in prj.conf
CONFIG_AKIRA_AUDIO=n
CONFIG_PWM=n
```

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
