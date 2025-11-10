# Build Scripts Guide

This document explains the build and flash scripts available in AkiraOS and how to use them with different platforms.

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

## See Also

- [BUILD_PLATFORMS.md](BUILD_PLATFORMS.md) - Detailed platform comparison and selection guide
- [SENSOR_INTEGRATION.md](SENSOR_INTEGRATION.md) - Sensor module integration guide
- [INTEGRATION_GUIDE.md](INTEGRATION_GUIDE.md) - Module integration guide
- [troubleshooting.md](troubleshooting.md) - General troubleshooting guide
