# AkiraOS Quick Start Guide

<div align="center">

**Get AkiraOS running in under 15 minutes**

*Complete setup guide from zero to flashing your first firmware*

</div>

---

## 📋 Prerequisites

Before starting, ensure you have the following installed:

| Requirement | Version | Installation |
|-------------|---------|--------------|
| **Linux/WSL2** | Ubuntu 20.04+ | [WSL Setup](https://docs.microsoft.com/windows/wsl/install) |
| **Python** | 3.8+ | `sudo apt install python3 python3-pip` |
| **Git** | 2.25+ | `sudo apt install git` |
| **CMake** | 3.20+ | `sudo apt install cmake` |
| **Device Tree Compiler** | 1.4+ | `sudo apt install device-tree-compiler` |
| **West** | Latest | `pip3 install west` |

### System Dependencies

```bash
# Install required packages
sudo apt update
sudo apt install --no-install-recommends git cmake ninja-build gperf \
  ccache dfu-util device-tree-compiler wget \
  python3-dev python3-pip python3-setuptools python3-tk python3-wheel \
  xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev \
  libmagic1

# Install Python packages
pip3 install --user -U west
pip3 install pyelftools

# Add Python user bin to PATH (add to ~/.bashrc for persistence)
export PATH="$HOME/.local/bin:$PATH"
```

---

## 🚀 First Time Setup

### Step 1: Create Workspace

AkiraOS uses a **West workspace** structure where dependencies live alongside your project.

```bash
# Create workspace directory
cd ~
mkdir akira-workspace && cd akira-workspace

# Clone AkiraOS repository
git clone --recursive https://github.com/ArturR0k3r/AkiraOS.git
cd AkiraOS
```

**Directory structure:**
```
~/akira-workspace/
└── AkiraOS/          # Your application code (you are here)
    ├── src/
    ├── boards/
    ├── modules/
    └── west.yml      # Dependency manifest
```

---

### Step 2: Initialize West Workspace

West will fetch **Zephyr RTOS**, **WAMR**, and all required modules.

```bash
# Initialize west workspace (from AkiraOS directory)
west init -l .

# Move to workspace root
cd ..

# Fetch all dependencies (~500MB download)
west update
```

**What `west update` fetches:**
- ✅ Zephyr RTOS v4.3.0
- ✅ MCUboot bootloader
- ✅ HAL drivers (ESP-IDF, nRF, STM32)
- ✅ Build tools and scripts

**After `west update`, your workspace looks like:**
```
~/akira-workspace/
├── AkiraOS/              # Your application
├── zephyr/               # Zephyr RTOS
├── bootloader/           # MCUboot
├── modules/              # Zephyr modules
└── tools/                # Build utilities
```

**⏱️ Expected time:** ~5-10 minutes depending on internet speed

---

### Step 3: Install Zephyr SDK

The Zephyr SDK provides cross-compilers for all supported architectures.

```bash
cd ~

# Download SDK (choose version 0.17.0 for Zephyr 4.3.0)
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0/zephyr-sdk-0.17.0_linux-x86_64.tar.xz

# Extract (~2GB extracted)
tar xvf zephyr-sdk-0.17.0_linux-x86_64.tar.xz

# Run setup script
```

---

## 🧪 Testing Your Build

### Native Simulation (Fastest Testing)

Native simulation runs AkiraOS on your host CPU—no hardware required!

```bash
cd ~/akira-workspace/AkiraOS

# Build for native_sim
./build.sh -b native_sim

# Run the executable
cd ../build/zephyr
./zephyr.exe
```

**Advantages:**
- ✅ Instant feedback (no flashing)
- ✅ GDB debugging support
- ✅ Test core logic without hardware

**Limitations:**
- ❌ No real WiFi/Bluetooth
- ❌ No hardware peripherals
- ❌ Simulated timing only

---

## 📊 Platform Comparison

| Platform | CPU | RAM | Flash | PSRAM | WiFi | BLE | USB | Recommended Use |
|----------|-----|-----|-------|-------|------|-----|-----|-----------------|
| **ESP32-S3** | Dual Xtensa @ 240MHz | 512KB | 8MB | ✅ 8MB | ✅ | ✅ | ✅ | **Primary dev target** |
| **ESP32** | Dual Xtensa @ 240MHz | 520KB | 4MB | ⚠️ Limited | ✅ | ✅ | ❌ | Legacy support |
| **ESP32-C3** | RISC-V @ 160MHz | 400KB | 4MB | ❌ | ✅ | ✅ | ✅ | Low-cost option |
| **native_sim** | Host CPU | Host | N/A | N/A | ❌ | ❌ | ❌ | Fast testing |

**💡 Recommendation:** Start with **ESP32-S3** for full feature support and best performance.

---

## 🔧 Configuration Management

### Kconfig System

AkiraOS uses Zephyr's Kconfig for build-time configuration.

#### Global Configuration

Edit **[prj.conf](prj.conf)** for settings that apply to all platforms:
```

---

## 📁 Workspace Structure Reference

After completing setup, your workspace should look like this:

```
~/akira-workspace/              # West workspace root
│
├── AkiraOS/                    # Your application (this repo)
│   ├── src/                    # Application source code
│   │   ├── main.c
│   │   ├── runtime/            # WAMR integration
│   │   ├── connectivity/       # Network stack
│   │   └── api/                # Native API bindings
│   ├── boards/                 # Board-specific configs
│   │   ├── esp32s3_devkitm_esp32s3_procpu.conf
│   │   ├── esp32s3_devkitm_esp32s3_procpu.overlay
│   │   └── ...
│   ├── include/                # Public headers
│   ├── modules/                # Submodules
│   │   └── wasm-micro-runtime/ # WAMR (git submodule)
│   ├── wasm_sample/            # Example WASM apps
│   ├── CMakeLists.txt          # Build configuration
│   ├── prj.conf                # Global Kconfig
│   ├── west.yml                # Dependency manifest
│   └── build.sh                # Build script
│and Maintaining

### Update Zephyr and Modules

```bash
cd ~/akira-workspace
west update

# If dependencies change, re-fetch blobs
west blobs fetch hal_espressif
```

### Update AkiraOS

```bash
cd ~/akira-workspace/AkiraOS

# Pull latest changes
git pull origin main

# UpCommon Issues

#### 1. "west: command not found"

**Cause:** West not in PATH after installation

**Solution:**
```bash
# Install west with user flag
pip3 install --user west

# Add to PATH permanently
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc

# Verify installation
west --version
```

---

#### 2. "Permission denied" when flashing

**Cause:** User not in `dialout` group for serial port access

**Solution:**
```bash
# Add user to dialout group
sudo usermod -a -G dialout $USER

# Check group membership
groups

# Log out and back in for changes to take effect
# Or run: newgrp dialout
```

**Temporary workaround:**
```bash
# Use sudo (not recommended for regular use)
sudo west flash
```

---

### Hardware Comparison

| Platform | CPU | RAM | PSRAM | WiFi | BLE | USB | Cost | Best For |
|----------|-----|-----|-------|------|-----|-----|------|----------|
| **ESP32-S3** | 2x Xtensa @ 240MHz | 512KB | 8MB | ✅ | ✅ | ✅ | ~$5 | **Production** |
| **ESP32** | 2x Xtensa @ 240MHz | 520KB | Limited | ✅ | ✅ | ❌ | ~$3 | Legacy |
| **ESP32-C3** | RISC-V @ 160MHz | 400KB | ❌ | ✅ | ✅ | ✅ | ~$2 | Cost-sensitive |
| **native_sim** | Host CPU | Host | N/A | ❌ | ❌ | ❌ | Free | Testing |

### Recommended: ESP32-S3 DevKitM

**Why ESP32-S3?**
- ✅ **8MB PSRAM** – Essential for WASM applications
- ✅ **Dual-core** – Real-time performance
- ✅ **USB OTG** – Native USB support
- ✅ **WiFi 4 + BLE 5** – Full connectivity
- ✅ � Next Steps

### 1. Explore the System

Once booted, try these shell commands:

```bash
# System information
uart:~$ kernel version
uart:~$ kernel uptime

# Runtime status
uart:~$ wasm status

# Network status
uart:~$ net iface

# File system
uart:~$ fs ls /

# Help
uart:~$ help
```

### 2. Upload a WASM Application

```bash
# On your PC, build a WASM app
cd ~/akira-workspace/AkiraOS/wasm_sample
./build_wasm_apps.sh

# Upload via HTTP (ESP32 must be connected to WiFi)
curl -X POST -F "file=@example_app.wasm" http://<esp32-ip>/upload
```

### 3. Perform OTA Update

```bash
# Build new firmware
cd ~/akira-workspace/AkiraOS
./build.sh -b esp32s3_devkitm_esp32s3_procpu

# Upload via OTA endpoint
curl -X POST -F "firmware=@../build/zephyr/zephyr.bin" \
  http://<esp32-ip>/ota/upload
```

### 4. Dive Deeper

| Topic | Documentation | Description |
|-------|---------------|-------------|
| **Architecture** | [Architecture.md](docs/Architecture.md) | System design and data flows |
| **Runtime** | [Runtime README](src/runtime/README.md) | WAMR integration and execution model |
| **Connectivity** | [Connectivity README](src/connectivity/README.md) | Network stack and protocols |
| **Hardware** | [Hardware.md](docs/Hardware.md) | Supported boards and peripherals |
| **Contributing** | [CONTRIBUTING.md](CONTRIBUTING.md) | How to contribute to AkiraOS |

---

## 💡 Pro Tips

### Development Workflow

1. **Use native_sim for rapid iteration**
   ```bash
   # No flashing needed!
   ./build.sh -b native_sim && ../build/zephyr/zephyr.exe
   ```

2. **Enable debug logging**
   ```bash
   # Edit prj.conf
   CONFIG_LOG_DEFAULT_LEVEL=4  # 4 = DEBUG
   ```

3. **Use menuconfig for exploration**
   ```bash
   west build -t menuconfig
   # Browse available options, save changes
   ```

4. **Monitor WiFi credentials**
   ```bash
   # Set via Kconfig
   CONFIG_WIFI_SSID="YourNetwork"
   CONFIG_WIFI_PSK="YourPassword"
   ```

5. **Quick reflash without rebuild**
   ```bash
   # Just flash existing binary
   west flash
   ```

### Performance Tuning

```bash
# Increase PSRAM heap for large WASM apps
CONFIG_HEAP_MEM_POOL_SIZE=262144  # 256KB

# Enable AOT compilation for 2x speed
CONFIG_WAMR_BUILD_AOT=y

# Optimize for size
CONFIG_SIZE_OPTIMIZATIONS=y
```

### Debugging

```bash
# Enable thread stack monitoring
CONFIG_THREAD_STACK_INFO=y
CONFIG_INIT_STACKS=y

# Enable assert
CONFIG_ASSERT=y

# Core dumps on crash
CONFIG_DEBUG_COREDUMP=y
```

---

## 🎓 Learning Resources

### Official Documentation

- **Zephyr Docs:** https://docs.zephyrproject.org/
- **WAMR Docs:** https://github.com/bytecodealliance/wasm-micro-runtime/tree/main/doc
- **ESP-IDF:** https://docs.espressif.com/projects/esp-idf/

### Community

- **AkiraOS Discussions:** [GitHub Discussions](https://github.com/ArturR0k3r/AkiraOS/discussions)
- **Zephyr Discord:** https://chat.zephyrproject.org/
- **ESP32 Forum:** https://esp32.com/

---

## ✅ Checklist

Before you start developing, make sure:

- [ ] West workspace initialized (`west update` completed)
- [ ] Zephyr SDK installed and in PATH
- [ ] Python dependencies installed
- [ ] ESP-IDF blobs fetched (for ESP32 platforms)
- [ ] User added to `dialout` group
- [ ] First build successful
- [ ] Hardware flashed and booting
- [ ] Serial console accessible

---

<div align="center">

**🎉 Congratulations! You're ready to develop with AkiraOS!**

Questions? Open an issue on [GitHub](https://github.com/ArturR0k3r/AkiraOS/issues)

[⬆ Back to Top](#akiraos-quick-start-guide)

</div>
│  [40 GPIO]      │          │  [22 GPIO]      │
└─────────────────┘          └─────────────────┘
   Recommended                 Budget Option
```mmand 'cmake' not found"

**Cause:** Missing build tools

**Solution:**
```bash
sudo apt update
sudo apt install cmake ninja-build
```

---

#### 5. "west update" fails with network errors

**Cause:** Slow/unstable internet connection

**Solution:**
```bash
# Retry with verbose output
west update -v

# Or manually clone failing repos
cd ~/akira-workspace
git clone https://github.com/zephyrproject-rtos/zephyr.git
west update
```

---

#### 6. ESP32 flash fails with "Failed to connect"

**Cause:** Board not in download mode or wrong serial port

**Solution:**
```bash
# 1. Verify serial port
ls -l /dev/ttyUSB* /dev/ttyACM*

# 2. Manually enter download mode
# Hold BOOT button, press RESET, release BOOT

# 3. Flash with explicit port
west flash --port /dev/ttyUSB0

# 4. Check USB cable (must support data, not just power)
```

---

#### 7. Build fails with submodule errors

**Cause:** WAMR submodule not initialized

**Solution:**
```bash
cd ~/akira-workspace/AkiraOS

# Force submodule reinitialization
git submodule deinit -f modules/wasm-micro-runtime
git submodule update --init --recursive --force

# Verify submodule status
git submodule status
```

---

#### 8. "ZEPHYR_BASE not set" error

**Cause:** West workspace not properly initialized

**Solution:**
```bash
cd ~/akira-workspace/AkiraOS

# Reinitialize west workspace
west init -l .
cd ..
west update
```

---

#### 9. Out of memory during build

**Cause:** Insufficient RAM for parallel builds

**Solution:**
```bash
# Limit ninja parallel jobs
west build -b esp32s3_devkitm_esp32s3_procpu AkiraOS -- -j2

# Or use build script with lower parallelism
MAKEFLAGS="-j2" ./build.sh -b esp32s3_devkitm_esp32s3_procpu
```

---

#### 10. Native sim crashes on startup

**Cause:** Missing SDL2 libraries for display simulation

**Solution:**
```bash
sudo apt install libsdl2-dev

# Rebuild
cd ~/akira-workspace/AkiraOS
./build.sh -b native_sim -r all
```

---

### Getting More Help

If you encounter issues not listed here:

1. **Check build logs:** `~/akira-workspace/build/build.log`
2. **Enable verbose output:** `west -v build`
3. **Search issues:** [GitHub Issues](https://github.com/ArturR0k3r/AkiraOS/issues)
4. **Ask for help:** [GitHub Discussions](https://github.com/ArturR0k3r/AkiraOS/discussions)

**When reporting issues, include:**
- OS and version (`uname -a`)
- West version (`west --version`)
- Build command and full output
- Board type`
# Edit boards/esp32s3_devkitm_esp32s3_procpu.conf
CONFIG_NET_SOCKETS_SERVICE_STACK_SIZE=8192
```

#### Interactive Configuration

```bash
cd ~/akira-workspace/AkiraOS

# Open menuconfig TUI
west build -t menuconfig

# Save changes to prj.conf
# Changes are automatically applied on next build

#### Quick Build (ESP32-S3)

```bash
cd ~/akira-workspace/AkiraOS

# Build for ESP32-S3 DevKitM
./build.sh -b esp32s3_devkitm_esp32s3_procpu
```

**Build output:**
```
~/akira-workspace/build/
├── zephyr/
│   ├── zephyr.elf      # ELF binary
│   ├── zephyr.bin      # Flashable firmware
│   └── merged.hex      # Combined bootloader + app
└── compile_commands.json
```

**⏱️ Expected time:** 2-3 minutes (first build)

#### Build for Other Platforms

```bash
# ESP32 (classic)
./build.sh -b esp32_devkitc_procpu

# ESP32-C3 (RISC-V)
./build.sh -b esp32c3_devkitm

# Native simulation
./build.sh -b native_sim
```

---

### Step 7: Flash to Hardware

#### Connect Your Device

1. Connect ESP32 board via USB
2. Identify serial port:
   ```bash
   ls /dev/ttyUSB* /dev/ttyACM*
   # Should show /dev/ttyUSB0 or similar
   ```

#### Flash Firmware

```bash
cd ~/akira-workspace/AkiraOS

# Auto-detect and flash
west flash

# Or specify port manually
west flash --port /dev/ttyUSB0
```

**⏱️ Expected time:** ~30 seconds

---

### Step 8: Verify Boot

```bash
# Monitor serial output
west espmonitor

# Or use picocom/screen
picocom -b 115200 /dev/ttyUSB0
```

**Expected boot output:**
```
*** Booting Zephyr OS build v4.3.0 ***
[00:00:00.123,000] <inf> main: AkiraOS v1.4.7 starting...
[00:00:00.234,000] <inf> runtime: WAMR initialized (AOT enabled)
[00:00:00.345,000] <inf> connectivity: WiFi stack ready
[00:00:00.456,000] <inf> main: System initialized successfully

uart:~$
```

**Press Tab** to see available shell commands.

---

## 🔨 Build System Deep Dive

### Build Script Options

```bash
# Full rebuild (clean first)
./build.sh -b esp32s3_devkitm_esp32s3_procpu -r all

# Pristine build (delete build directory)
./build.sh -b esp32s3_devkitm_esp32s3_procpu -p

# Build with custom config
./build.sh -b esp32s3_devkitm_esp32s3_procpu -- -DCONFIG_HEAP_MEM_POOL_SIZE=262144
```

### Manual West Commands

For advanced users who prefer direct `west` control:

```bash
cd ~/akira-workspace

# Build
west build -b esp32s3_devkitm_esp32s3_procpu AkiraOS -d build

# Flash
west flash -d build

# Clean
west build -t clean -d build

# Pristine (delete build)
rm -rf build

---

## 🔨 Build Commands

### Build All Platforms

```bash
./build_all.sh           # Build all platforms (native_sim, ESP32-S3, ESP32, ESP32-C3)
```

**Individual platform builds:**
The `build_all.sh` script automatically builds all four targets. To build specific platforms, 
use west commands directly or modify the script.

### Build with MCUboot Bootloader

```bash
./build_both.sh esp32s3        # Build bootloader + app
./build_both.sh esp32s3 clean  # Clean and build
./build_both.sh esp32          # Build for ESP32
./build_both.sh esp32c3        # Build for ESP32-C3
```

### Flash to Hardware

```bash
# Auto-detect chip type and flash
./flash.sh

# Flash specific platform
./flash.sh --platform esp32s3
./flash.sh --platform esp32
./flash.sh --platform esp32c3

# Flash only application (faster updates)
./flash.sh --app-only

# Specify serial port
./flash.sh --port /dev/ttyUSB0
```

### Native Simulation

```bash
# Build and run in one command
./build_and_run.sh

# Or manually
./build_all.sh
./build_native_sim/zephyr/zephyr.exe
```

**Note:** The build creates a `build_native_sim` directory at the workspace root 
(`<workspace>/build_native_sim`), not inside the AkiraOS directory.

---

## 📁 Workspace Structure After Setup

```
<workspace>/  (e.g., ~/akira-workspace/)
├── AkiraOS/                    # Your application code
│   ├── src/                    # Application source
│   ├── boards/                 # Board-specific overlays
│   ├── modules/                # Local modules
│   │   └── wasm-micro-runtime/ # WAMR module
│   ├── build_*.sh              # Build scripts
│   ├── flash.sh                # Flash script
│   └── west.yml                # West manifest
├── build_native_sim/           # Native sim build output
├── build_esp32s3/              # ESP32-S3 build output
├── build_esp32/                # ESP32 build output
├── build_esp32c3/              # ESP32-C3 build output
├── zephyr/                     # Zephyr RTOS (fetched by west)
├── wasm-micro-runtime/         # WAMR submodule 
├── bootloader/                 # MCUboot (fetched by west)
├── modules/                    # Zephyr modules (fetched by west)
└── tools/                      # Build tools (fetched by west)
```

**Important:** Build directories are created at workspace root (`<workspace>/`), not inside AkiraOS.

---

## 🔄 Updating Dependencies

### Update Zephyr and Modules

```bash
cd <workspace>  # Your workspace directory
west update
```

### Update WASM-Micro-Runtime Submodule

```bash
cd <workspace>/AkiraOS
git submodule update --remote modules/wasm-micro-runtime
```

---

## 🐛 Troubleshooting

### "west: command not found"

```bash
pip3 install west
# Or if that fails:
pip3 install --user west
export PATH="$HOME/.local/bin:$PATH"
```

### "Permission denied" on Flash

```bash
# Add user to dialout group
sudo usermod -a -G dialout $USER
# Log out and back in for changes to take effect
```

### "No module named 'elftools'"

```bash
pip3 install pyelftools
```

### Build Fails with Submodule Errors

```bash
# Re-initialize all submodules
cd <workspace>/AkiraOS
git submodule update --init --recursive --force
```

### Clean Everything and Rebuild

```bash
cd <workspace>/AkiraOS
rm -rf ../build_*
./build_all.sh
```

**Note:** Build directories are at workspace root (`<workspace>/build_*`), so clean from there.

---

## 🎯 Platform Selection Guide

| Platform | Use Case | Memory | Cores |
|----------|----------|--------|-------|
| **ESP32-S3** | Primary Console | 512KB RAM + 8MB PSRAM | Dual Xtensa |
| **ESP32** | Legacy Console | 520KB RAM | Dual Xtensa |
| **ESP32-C3** | Remote Modules | 400KB RAM | Single RISC-V |
| **native_sim** | Development/Testing | Host memory | Host CPU |

### Recommended: ESP32-S3

- Best performance and memory
- Full feature support
- PSRAM for WASM applications
- Primary development target

---

## 📚 Next Steps

After setup, check out:

- **[README.md](README.md)** - Project overview
- **[docs/api-reference.md](docs/api-reference.md)** - System APIs
- **[docs/AkiraOS.md](docs/AkiraOS.md)** - Architecture details

---

## 💡 Pro Tips

1. **Use native_sim for fast development** - No hardware needed, instant feedback
2. **Use `--app-only` for faster flashing** - Skip bootloader when testing app changes
3. **Monitor serial output** - Use `west espmonitor` for better output formatting
4. **Clean builds when switching platforms** - Run `./build_both.sh <platform> clean`

---

**Need help?** Open an issue on [GitHub](https://github.com/ArturR0k3r/AkiraOS/issues)
