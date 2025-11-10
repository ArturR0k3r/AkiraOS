# Build Scripts Guide

This document explains the build and flash scripts available in AkiraOS and how to use them with different platforms.

## Build and Flash Workflow

```
┌─────────────────────────────────────────────────────────────────────┐
│                        AkiraOS Build Workflow                        │
└─────────────────────────────────────────────────────────────────────┘

                         Choose Your Path:
                                 │
                ┌────────────────┼────────────────┐
                │                │                │
                ▼                ▼                ▼
        ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
        │  ESP32-S3    │ │  ESP32-C3    │ │ Native Sim   │
        │   Console    │ │   Modules    │ │   Testing    │
        │  (Primary)   │ │   (Only)     │ │              │
        └──────┬───────┘ └──────┬───────┘ └──────┬───────┘
               │                │                │
               ▼                ▼                ▼
        ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
        │build_both.sh │ │build_both.sh │ │build_and_run │
        │   esp32s3    │ │   esp32c3    │ │     .sh      │
        └──────┬───────┘ └──────┬───────┘ └──────┬───────┘
               │                │                │
               ▼                ▼                │
        ┌──────────────┐ ┌──────────────┐       │
        │   MCUboot    │ │   MCUboot    │       │
        │  Bootloader  │ │  Bootloader  │       │
        └──────┬───────┘ └──────┬───────┘       │
               │                │                │
               ▼                ▼                │
        ┌──────────────┐ ┌──────────────┐       │
        │   AkiraOS    │ │   AkiraOS    │       │
        │ Application  │ │ Application  │       │
        └──────┬───────┘ └──────┬───────┘       │
               │                │                │
               ▼                ▼                ▼
        ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
        │  flash.sh    │ │  flash.sh    │ │  ./zephyr    │
        │ --platform   │ │ --platform   │ │    .exe      │
        │   esp32s3    │ │   esp32c3    │ │              │
        └──────┬───────┘ └──────┬───────┘ └──────┬───────┘
               │                │                │
               ▼                ▼                ▼
        ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
        │   ESP32-S3   │ │   ESP32-C3   │ │   Console    │
        │    Device    │ │    Device    │ │   Output     │
        │  (Console)   │ │  (Modules)   │ │              │
        └──────┬───────┘ └──────┬───────┘ └──────────────┘
               │                │
               ▼                ▼
        ┌──────────────┐ ┌──────────────┐
        │     west     │ │     west     │
        │  espmonitor  │ │  espmonitor  │
        └──────────────┘ └──────────────┘
```

---

## Available Scripts

### 1. build_all.sh - Multi-Platform Build Script

Builds AkiraOS for one or more platforms.

**Usage:**
```bash
./build_all.sh [platform]
```

**Platforms:**
- `all` - Build for all platforms (default)
- `native_sim` - Native x86_64 simulation
- `esp32s3` - ESP32-S3 DevKitM (Akira Console - Primary)
- `esp32` - ESP32 DevKitC (Akira Console - Legacy)
- `esp32c3` - ESP32-C3 DevKitM (Akira Modules Only)

**Examples:**
```bash
# Build all platforms
./build_all.sh
./build_all.sh all

# Build specific platform
./build_all.sh esp32s3
./build_all.sh esp32c3
./build_all.sh native_sim

# Show help
./build_all.sh help
```

**Output:**
- `build-native-sim/` - Native simulation build
- `build-esp32s3/` - ESP32-S3 build
- `build-esp32/` - ESP32 build
- `build-esp32c3/` - ESP32-C3 build

---

### 2. build_both.sh - MCUboot + AkiraOS Build Script

Builds both MCUboot bootloader and AkiraOS application for a specific ESP32 platform.

**Usage:**
```bash
./build_both.sh [platform] [clean]
```

**Platforms:**
- `esp32s3` - ESP32-S3 DevKitM (default, Akira Console)
- `esp32` - ESP32 DevKitC (Akira Console - Legacy)
- `esp32c3` - ESP32-C3 DevKitM (Akira Modules Only)

**Options:**
- `clean` - Clean build directories before building

**Examples:**
```bash
# Build for ESP32-S3 (default)
./build_both.sh
./build_both.sh esp32s3

# Clean and build for ESP32-S3
./build_both.sh esp32s3 clean
./build_both.sh clean    # Uses default platform

# Build for ESP32-C3 (Akira Modules)
./build_both.sh esp32c3

# Show help
./build_both.sh help
```

**Output:**
- `build-mcuboot/` - MCUboot bootloader
- `build/` - AkiraOS application

**Important Notes:**
- ESP32-C3 is for **Akira Modules Only**, not Akira Console
- Use this script when you need secure boot with MCUboot
- MCUboot provides OTA update capabilities

---

### 3. flash.sh - Flash to ESP32 Devices

Flashes MCUboot and/or AkiraOS to ESP32 devices with automatic chip detection.

**Usage:**
```bash
./flash.sh [OPTIONS]
```

**Options:**
- `--platform PLATFORM` - Specify platform (esp32, esp32s3, esp32c3)
- `--bootloader-only` - Flash only MCUboot
- `--app-only` - Flash only AkiraOS application
- `--port PORT` - Specify serial port (default: auto-detect)
- `--baud BAUD` - Specify baud rate (default: 921600)
- `--help` - Show help message

**Examples:**
```bash
# Auto-detect and flash both MCUboot and AkiraOS
./flash.sh

# Flash to specific platform
./flash.sh --platform esp32s3
./flash.sh --platform esp32c3

# Flash only application
./flash.sh --app-only

# Flash to specific port
./flash.sh --port /dev/ttyUSB0

# Flash only bootloader to ESP32-S3
./flash.sh --platform esp32s3 --bootloader-only
```

**Flash Addresses:**
- MCUboot: `0x1000`
- AkiraOS: `0x20000`

**Chip Detection:**
The script automatically detects the connected chip type:
- ESP32-S3 → Akira Console (Primary)
- ESP32 → Akira Console (Legacy)
- ESP32-C3 → Akira Modules Only

**Important Notes:**
- ESP32-C3 is for **Akira Modules Only**, not Akira Console
- Script warns when flashing to ESP32-C3
- Supports auto-detection of serial port

---

### 4. build_and_run.sh - Build and Execute

Builds and runs AkiraOS on native simulation (x86_64).

**Usage:**
```bash
./build_and_run.sh
```

**Features:**
- Builds for native_sim platform
- Automatically runs the executable
- No arguments needed (native_sim only)

---

## Platform Selection Guide

### When to Use Each Platform

| Platform | Use Case | Console | Modules | Memory |
|----------|----------|---------|---------|--------|
| **ESP32-S3** | Primary Akira Console | ✅ Yes | ✅ Yes | 512KB SRAM + 8MB PSRAM |
| **ESP32** | Legacy Akira Console | ✅ Yes | ✅ Yes | 520KB SRAM + 4MB Flash |
| **ESP32-C3** | **Akira Modules Only** | ❌ No | ✅ Yes | 400KB SRAM |
| **native_sim** | Development/Testing | N/A | N/A | Host memory |

### Why ESP32-C3 is Modules Only

The ESP32-C3 has the following limitations that prevent it from being used as an Akira Console:

1. **Single-core RISC-V** - Cannot handle UI + background tasks efficiently
2. **No PSRAM** - Insufficient memory for display framebuffer
3. **Limited GPIO** - Not enough pins for full Console hardware
4. **Lower clock speed** - 160 MHz vs 240 MHz on ESP32-S3

However, ESP32-C3 is excellent for **Akira Modules**:
- ✅ Sufficient for sensor drivers (NRF24L01, LSM6DS3, INA219)
- ✅ Low power consumption
- ✅ Cost-effective
- ✅ Good for remote/wireless modules

See [BUILD_PLATFORMS.md](BUILD_PLATFORMS.md) for detailed platform information.

---

## Common Workflows

### Workflow 1: Build and Test on Native Simulation

```bash
# Quick build and run for testing
./build_and_run.sh
```

### Workflow 2: Build for All Platforms

```bash
# Build everything
./build_all.sh

# Or build everything from scratch
cd ..
rm -rf build-*
cd AkiraOS
./build_all.sh
```

### Workflow 3: Build and Flash ESP32-S3 Console

```bash
# Build MCUboot + AkiraOS for ESP32-S3
./build_both.sh esp32s3

# Flash to device
./flash.sh --platform esp32s3

# Or let it auto-detect
./flash.sh
```

### Workflow 4: Build and Flash ESP32-C3 Module

```bash
# Build for ESP32-C3
./build_both.sh esp32c3

# Flash to device
./flash.sh --platform esp32c3
```

### Workflow 5: Quick Application Update

```bash
# Build only application (faster)
./build_all.sh esp32s3

# Flash only application (faster)
./flash.sh --app-only --platform esp32s3
```

### Workflow 6: Clean Build Specific Platform

```bash
# Clean and rebuild ESP32-S3
./build_both.sh esp32s3 clean

# Or manually
rm -rf ../build-mcuboot ../build
./build_both.sh esp32s3
```

---

## Build Output Structure

```
Akira/
├── AkiraOS/                    # Source code
│   ├── build_all.sh            # Multi-platform builder
│   ├── build_both.sh           # MCUboot + AkiraOS builder
│   ├── flash.sh                # Flash script
│   └── build_and_run.sh        # Native sim runner
│
├── build/                      # Single platform build (legacy)
│   └── zephyr/
│       ├── zephyr.elf
│       ├── zephyr.bin
│       └── zephyr.signed.bin
│
├── build-native-sim/           # Native simulation build
│   └── zephyr/
│       └── zephyr.exe
│
├── build-esp32s3/              # ESP32-S3 build
│   └── zephyr/
│       ├── zephyr.elf
│       ├── zephyr.bin
│       └── zephyr.signed.bin
│
├── build-esp32/                # ESP32 build
│   └── zephyr/
│       ├── zephyr.elf
│       ├── zephyr.bin
│       └── zephyr.signed.bin
│
├── build-esp32c3/              # ESP32-C3 build (Modules Only)
│   └── zephyr/
│       ├── zephyr.elf
│       ├── zephyr.bin
│       └── zephyr.signed.bin
│
└── build-mcuboot/              # MCUboot bootloader
    └── zephyr/
        └── zephyr.bin
```

---

## Monitoring Serial Output

After flashing, monitor the serial output:

### Using west espmonitor (Recommended)
```bash
west espmonitor --port /dev/ttyUSB0
```

### Using screen
```bash
screen /dev/ttyUSB0 115200
# Exit: Ctrl+A then K then Y
```

### Using picocom
```bash
picocom -b 115200 /dev/ttyUSB0
# Exit: Ctrl+A then Ctrl+X
```

### Using minicom
```bash
minicom -D /dev/ttyUSB0 -b 115200
# Exit: Ctrl+A then X
```

---

## Troubleshooting

### Build fails with "No such file or directory"

**Solution:** Make sure you're in the AkiraOS directory:
```bash
cd /home/artur_ubuntu/Akira/AkiraOS
./build_all.sh
```

### Flash fails with "No serial port detected"

**Solution:** Specify the port manually:
```bash
./flash.sh --port /dev/ttyUSB0
```

### ESP32-C3 won't flash or crashes

**Problem:** You might be trying to use Console features on ESP32-C3.

**Solution:** ESP32-C3 is for Akira Modules only! Use ESP32-S3 for Console:
```bash
# Build for Akira Modules (ESP32-C3)
./build_both.sh esp32c3

# OR build for Akira Console (ESP32-S3)
./build_both.sh esp32s3
```

### Build directory conflicts

**Problem:** Multiple build directories with different platforms.

**Solution:** Clean and rebuild:
```bash
cd ..
rm -rf build build-* 
cd AkiraOS
./build_both.sh esp32s3 clean
```

### Wrong chip detected

**Solution:** Specify platform explicitly:
```bash
./flash.sh --platform esp32s3
```

### Permission denied on serial port

**Solution:** Add user to dialout group:
```bash
sudo usermod -a -G dialout $USER
# Then log out and back in
```

---

## Quick Reference

| Task | Command |
|------|---------|
| Build all platforms | `./build_all.sh` |
| Build ESP32-S3 Console | `./build_both.sh esp32s3` |
| Build ESP32-C3 Modules | `./build_both.sh esp32c3` |
| Clean build ESP32-S3 | `./build_both.sh esp32s3 clean` |
| Flash auto-detect | `./flash.sh` |
| Flash specific platform | `./flash.sh --platform esp32s3` |
| Flash app only | `./flash.sh --app-only` |
| Build and test native | `./build_and_run.sh` |
| Monitor serial | `west espmonitor` |

---

## Important Reminders

1. **ESP32-C3 is for Akira Modules Only** - It cannot run the full Akira Console due to hardware limitations
2. **ESP32-S3 is the Primary Console** - Use this for full Akira Console functionality
3. **ESP32 is Legacy Console** - Still supported but ESP32-S3 is recommended
4. **Always specify platform** - When building for a specific target, use the platform argument
5. **Check BUILD_PLATFORMS.md** - For detailed platform comparison and selection guide

---

## Step-by-Step Guides

### First-Time Setup

1. **Install Dependencies**
```bash
# Install esptool
pip install esptool

# Install west (if not already installed)
pip install west

# Verify installation
esptool version
west --version
```

2. **Clone and Initialize Workspace**
```bash
cd ~
mkdir Akira
cd Akira
git clone <your-repo-url> AkiraOS
cd AkiraOS

# Initialize west workspace
west init -l .
cd ..
west update
```

3. **Build for Your Platform**
```bash
cd AkiraOS

# For ESP32-S3 Console (recommended)
./build_both.sh esp32s3

# For ESP32-C3 Modules
./build_both.sh esp32c3

# For native testing
./build_and_run.sh
```

4. **Connect Device and Flash**
```bash
# Auto-detect and flash
./flash.sh

# Or specify platform
./flash.sh --platform esp32s3
```

5. **Monitor Output**
```bash
west espmonitor --port /dev/ttyUSB0
# Press Ctrl+] to exit
```

---

### Development Workflow

#### Scenario 1: Quick Code Changes (Application Only)

When you modify application code (not bootloader):

```bash
# 1. Build only the changed platform
./build_all.sh esp32s3

# 2. Flash only the application (faster!)
./flash.sh --app-only --platform esp32s3

# 3. Monitor
west espmonitor
```

**Time saved:** ~2 minutes vs full build+flash

---

#### Scenario 2: Full Clean Build (Fresh Start)

When you have build issues or changed configurations:

```bash
# 1. Clean everything
cd /home/artur_ubuntu/Akira
rm -rf build build-* AkiraOS/build

# 2. Clean and rebuild
cd AkiraOS
./build_both.sh esp32s3 clean

# 3. Flash everything
./flash.sh --platform esp32s3
```

---

#### Scenario 3: Multi-Platform Testing

When testing across all platforms:

```bash
# 1. Build all at once
./build_all.sh

# 2. Test native first
cd ../build-native-sim/zephyr
./zephyr.exe

# 3. Flash to ESP32-S3
cd ../../AkiraOS
./flash.sh --platform esp32s3 --port /dev/ttyUSB0

# 4. Flash to ESP32-C3 (different device)
./flash.sh --platform esp32c3 --port /dev/ttyUSB1

# 5. Compare results
west espmonitor --port /dev/ttyUSB0
# (in another terminal)
west espmonitor --port /dev/ttyUSB1
```

---

#### Scenario 4: Akira Module Development (ESP32-C3)

When developing sensor modules for ESP32-C3:

```bash
# 1. Build for ESP32-C3
./build_both.sh esp32c3

# 2. Flash to module
./flash.sh --platform esp32c3

# 3. Test sensor commands
west espmonitor
# In console:
akira> nrf24 init
akira> nrf24 status
akira> lsm6ds3 init
akira> lsm6ds3 read
akira> ina219 init
akira> ina219 read
```

**Remember:** ESP32-C3 is for modules only, not Console!

---

#### Scenario 5: OTA Update Testing

When testing Over-The-Air updates:

```bash
# 1. Build with MCUboot
./build_both.sh esp32s3

# 2. Flash initial version
./flash.sh --platform esp32s3

# 3. Modify code, rebuild
./build_both.sh esp32s3

# 4. Use OTA to update (in AkiraOS shell)
akira> ota update http://192.168.1.100/zephyr.signed.bin

# 5. Monitor update progress
west espmonitor
```

---

### Platform-Specific Guides

#### Building for ESP32-S3 Console (Primary Platform)

**Hardware:** ESP32-S3 DevKitM
**Use Case:** Full Akira Console with display, UI, sensors

```bash
# Complete build and flash
./build_both.sh esp32s3
./flash.sh --platform esp32s3

# Quick application updates
./build_all.sh esp32s3
./flash.sh --app-only --platform esp32s3

# Monitor with baud rate
west espmonitor --port /dev/ttyUSB0 --baud 115200
```

**Features Available:**
- ✅ ILI9341 Display (320x240)
- ✅ Full UI System
- ✅ Sensor Modules (NRF24L01, LSM6DS3, INA219)
- ✅ Wi-Fi/BLE
- ✅ OTA Updates
- ✅ WASM Runtime
- ✅ Shell Commands

---

#### Building for ESP32-C3 Modules (Sensor Platform)

**Hardware:** ESP32-C3 DevKitM
**Use Case:** Remote sensor modules, wireless peripherals

```bash
# Build for modules
./build_both.sh esp32c3
./flash.sh --platform esp32c3

# Or just build (no MCUboot)
./build_all.sh esp32c3
```

**Features Available:**
- ✅ Sensor Modules (NRF24L01, LSM6DS3, INA219)
- ✅ Wi-Fi/BLE
- ✅ Shell Commands
- ✅ Low Power Mode
- ❌ No Display Support
- ❌ No UI System
- ❌ Limited WASM (memory constraints)

**Important:** ESP32-C3 cannot run Akira Console due to:
- Single-core (vs dual-core)
- No PSRAM (no framebuffer)
- Limited GPIO
- Lower clock speed

---

#### Building for Native Simulation (Development)

**Platform:** x86_64 Linux
**Use Case:** Fast testing, debugging, CI/CD

```bash
# Quick build and run
./build_and_run.sh

# Or manual
./build_all.sh native_sim
cd ../build-native-sim/zephyr
./zephyr.exe
```

**Features Available:**
- ✅ Core logic testing
- ✅ Shell commands
- ✅ Module APIs
- ✅ Fast iteration
- ❌ No real hardware
- ❌ No display
- ❌ No sensors

**Use Cases:**
- Unit testing
- Algorithm development
- Shell command testing
- CI/CD pipelines

---

### Advanced Topics

#### Custom Build Configurations

**Enable/Disable Sensors:**

Edit `prj.conf` or use board-specific configs:

```bash
# Enable all sensors
CONFIG_AKIRA_NRF24L01=y
CONFIG_AKIRA_LSM6DS3=y
CONFIG_AKIRA_INA219=y

# Disable a sensor
CONFIG_AKIRA_NRF24L01=n
```

**Custom Build:**
```bash
# Build with custom config
./build_all.sh esp32s3
```

---

#### Using Different Serial Ports

**Linux:**
```bash
# List available ports
ls /dev/ttyUSB* /dev/ttyACM*

# Flash to specific port
./flash.sh --port /dev/ttyUSB1 --platform esp32s3

# Monitor specific port
west espmonitor --port /dev/ttyUSB1
```

**Windows:**
```bash
# List COM ports
mode

# Flash to COM port
./flash.sh --port COM3 --platform esp32s3
```

**macOS:**
```bash
# List ports
ls /dev/cu.*

# Flash to port
./flash.sh --port /dev/cu.usbserial-0001 --platform esp32s3
```

---

#### Debugging Build Issues

**Problem:** Build fails with "MODULE_EXT_ROOT not found"

**Solution:**
```bash
cd /home/artur_ubuntu/Akira
# Make sure you're in the parent directory
west build --pristine -b esp32s3_devkitm/esp32s3/procpu AkiraOS -d build-esp32s3 -- -DMODULE_EXT_ROOT=$(pwd)/AkiraOS
```

---

**Problem:** Flash fails with "Permission denied on /dev/ttyUSB0"

**Solution:**
```bash
# Add user to dialout group
sudo usermod -a -G dialout $USER
# Log out and log back in

# Or use sudo (not recommended)
sudo ./flash.sh --platform esp32s3
```

---

**Problem:** ESP32-C3 crashes or won't flash

**Solution:**
ESP32-C3 has different memory and features. Check:
1. Are you trying to use display on ESP32-C3? (Not supported)
2. Are you using ESP32-C3 for Console? (Not supported)
3. Build specifically for esp32c3: `./build_both.sh esp32c3`

---

**Problem:** Multiple ESP32 devices connected

**Solution:**
```bash
# Identify devices
ls -la /dev/ttyUSB*
# or
esptool --port /dev/ttyUSB0 chip_id
esptool --port /dev/ttyUSB1 chip_id

# Flash to specific device
./flash.sh --port /dev/ttyUSB0 --platform esp32s3
./flash.sh --port /dev/ttyUSB1 --platform esp32c3
```

---

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Build AkiraOS

on:
  push:
    branches: [ main ]
  pull_request:
    branches: [ main ]

jobs:
  build-all-platforms:
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/zephyrproject-rtos/ci:latest
    
    steps:
      - name: Checkout
        uses: actions/checkout@v4
        with:
          path: AkiraOS
      
      - name: Setup West
        run: |
          cd AkiraOS
          west init -l .
          cd ..
          west update
      
      - name: Build All Platforms
        run: |
          cd AkiraOS
          ./build_all.sh
      
      - name: Upload Artifacts
        uses: actions/upload-artifact@v3
        with:
          name: akiraos-binaries
          path: |
            build-native-sim/zephyr/zephyr.exe
            build-esp32s3/zephyr/zephyr.bin
            build-esp32s3/zephyr/zephyr.signed.bin
            build-esp32/zephyr/zephyr.bin
            build-esp32c3/zephyr/zephyr.bin
```

---

## See Also

- [BUILD_PLATFORMS.md](BUILD_PLATFORMS.md) - Detailed platform comparison and selection guide
- [SENSOR_INTEGRATION.md](SENSOR_INTEGRATION.md) - Sensor module integration guide
- [INTEGRATION_GUIDE.md](INTEGRATION_GUIDE.md) - Module integration guide
- [troubleshooting.md](troubleshooting.md) - General troubleshooting guide
