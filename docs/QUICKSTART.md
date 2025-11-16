# AkiraOS Quick Start Guide

## Prerequisites
```bash
# Zephyr SDK 0.16.8 or later
# West 1.4.0 or later
# Python 3.8+
```

## Initial Setup (One Time)

### 1. Clone and Initialize Workspace
```bash
cd /home/artur_ubuntu/Akira/AkiraOS
west init -l .
cd ..
west update
west blobs fetch hal_espressif  # For ESP32 support
```

## Daily Development Workflow

### Build and Run (Native Simulator)
```bash
cd /home/artur_ubuntu/Akira/AkiraOS

# Option 1: Automated build and run
./build_and_run.sh

# Option 2: Manual steps
./build_all.sh native_sim
./build_native_sim/zephyr/zephyr.exe
```

### Build for Specific Platform
```bash
./build_all.sh native_sim   # Native simulator (✅ Working)
./build_all.sh esp32s3      # ESP32-S3 (⚠️ Currently broken)
./build_all.sh esp32        # ESP32 (⚠️ Currently broken)
./build_all.sh esp32c3      # ESP32-C3 (⚠️ Currently broken)
./build_all.sh all          # All platforms
```

### Flash to Hardware (When ESP32 Builds Work)
```bash
./flash.sh esp32s3  # Flash ESP32-S3
./flash.sh esp32    # Flash ESP32
./flash.sh esp32c3  # Flash ESP32-C3
```

## Interactive Shell Commands

Once running, use the `akira:~$` shell:

```bash
# System commands
kernel version      # Show Zephyr kernel version
kernel uptime       # Show system uptime
kernel threads      # List active threads

# Device commands
device list         # List all devices
sensor list         # List available sensors

# Help
help                # Show all available commands
```

## Development Tips

### Check Build Logs
```bash
# Native sim
cat /home/artur_ubuntu/Akira/build_native_sim/build.log

# ESP32-S3
cat /home/artur_ubuntu/Akira/build_esp32s3/build.log
```

### Clean Builds
```bash
# Clean specific build
rm -rf /home/artur_ubuntu/Akira/build_native_sim

# Clean all builds
rm -rf /home/artur_ubuntu/Akira/build_*
```

### Update Zephyr
```bash
cd /home/artur_ubuntu/Akira
west update
west blobs fetch hal_espressif  # Refresh ESP32 blobs if needed
```

## Current Platform Status

✅ **Native Simulator** - Fully working, use for development  
❌ **ESP32 Variants** - Build fails due to Zephyr 4.3.0 POSIX timer issue

See [BUILD_STATUS.md](BUILD_STATUS.md) for detailed status and workarounds.

## Troubleshooting

### "west: command not found"
```bash
pip3 install west
```

### "No module named 'west'"
```bash
pip3 install --upgrade west
```

### Build fails with module errors
```bash
cd /home/artur_ubuntu/Akira
west update
```

### ESP32 build fails
This is a known issue with Zephyr 4.3.0. Use native_sim for now.

### Native sim crashes immediately
Check that you're running from the correct directory:
```bash
cd /home/artur_ubuntu/Akira/build_native_sim/zephyr
./zephyr.exe
```

## Project Structure

```
AkiraOS/
├── src/                    # Application source code
│   ├── main.c             # Entry point
│   ├── akiraos.c          # Main OS logic
│   └── akira_modules/     # Feature modules
├── boards/                # Board-specific configs
├── docs/                  # Documentation
├── prj.conf              # Project configuration
├── CMakeLists.txt        # Build configuration
├── west.yml              # Manifest (Zephyr v4.3.0)
├── build_all.sh          # Multi-platform builder
├── build_and_run.sh      # Quick runner
└── flash.sh              # Hardware flasher
```

## Further Reading

- [BUILD_STATUS.md](BUILD_STATUS.md) - Current build status
- [BUILD_PLATFORMS.md](BUILD_PLATFORMS.md) - Platform support details
- [SENSOR_INTEGRATION.md](SENSOR_INTEGRATION.md) - Sensor setup
- [AkiraOS.md](AkiraOS.md) - Architecture overview
