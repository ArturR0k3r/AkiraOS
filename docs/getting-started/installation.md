---
layout: default
title: Installation
parent: Getting Started
nav_order: 1
---

# Installation Guide

Complete setup guide for AkiraOS development environment.

## System Requirements

- **OS:** Linux (Ubuntu 20.04+) or WSL2
- **Disk Space:** ~5GB (SDK + dependencies)
- **RAM:** 8GB+ recommended
- **Internet:** Broadband for downloading SDK (~2GB)

## Prerequisites

### Required Tools

```bash
# Update package database
sudo apt update

# Install core dependencies
sudo apt install --no-install-recommends git cmake ninja-build gperf \
  ccache dfu-util device-tree-compiler wget \
  python3-dev python3-pip python3-setuptools python3-tk python3-wheel \
  xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev \
  libmagic1

# Install Python tools
pip3 install --user -U west pyelftools

# Add Python user bin to PATH
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

**Verify installation:**
```bash
west --version    # Should show west 1.x.x
cmake --version   # Should show 3.20+
python3 --version # Should show 3.8+
```

## Workspace Setup

### Step 1: Create Directory Structure

```bash
# Create workspace
mkdir ~/akira-workspace && cd ~/akira-workspace

# Clone AkiraOS
git clone --recursive https://github.com/akiraos/AkiraOS.git
cd AkiraOS
```

### Step 2: Initialize West

```bash
# Initialize west workspace (from AkiraOS directory)
west init -l .

# Move to workspace root
cd ..

# Fetch Zephyr, MCUboot, and all modules (~500MB download)
west update
```

**Expected directory structure:**
```
~/akira-workspace/
├── AkiraOS/          # Application code
├── zephyr/           # Zephyr RTOS v4.3.0
├── bootloader/       # MCUboot
├── modules/          # Zephyr modules
└── tools/            # Build utilities
```

⏱️ **Time:** 5-10 minutes

### Step 3: Install Zephyr SDK

```bash
cd ~

# Download SDK v0.17.0 (for Zephyr 4.3.0)
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0/zephyr-sdk-0.17.0_linux-x86_64.tar.xz

# Extract
tar xvf zephyr-sdk-0.17.0_linux-x86_64.tar.xz

# Run setup script
cd zephyr-sdk-0.17.0
./setup.sh

# Register CMake package
./setup.sh -c

# Set environment variables (add to ~/.bashrc for persistence)
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.0
export ZEPHYR_BASE=~/akira-workspace/zephyr
```

⏱️ **Time:** 10-15 minutes

### Step 4: ESP32 Support (Platform-Specific)

For ESP32-S3 and other Espressif platforms:

```bash
cd ~/akira-workspace

# Fetch ESP-IDF HAL blobs
west blobs fetch hal_espressif
```

⏱️ **Time:** 2-3 minutes

## Verify Installation

### Test Native Simulation

```bash
cd ~/akira-workspace/AkiraOS

# Build for native_sim
./build.sh -b native_sim

# Run
cd ../build/zephyr
./zephyr.exe
```

**Expected output:**
```
*** Booting Zephyr OS build v4.3.0 ***
[00:00:00.000] <inf> main: AkiraOS v1.4.7 starting...
[00:00:00.010] <inf> wasm: Runtime initialized
uart:~$ 
```

Press `Ctrl+C` to exit.

## Hardware Setup

### USB Permissions (Linux)

```bash
# Add user to dialout group
sudo usermod -a -G dialout $USER

# Log out and back in, or run:
newgrp dialout

# Verify
groups  # Should show 'dialout'
```

### Connect Hardware

For **ESP32-S3 DevKitM:**

1. Connect via USB-C cable
2. Check device appears:
   ```bash
   ls /dev/ttyUSB* # or /dev/ttyACM*
   ```

3. Flash firmware:
   ```bash
   cd ~/akira-workspace/AkiraOS
   ./build.sh -b esp32s3_devkitm_esp32s3_procpu -r all
   ```

## Configuration

### WiFi Credentials (ESP32)

Edit board config file:
```bash
# boards/esp32s3_devkitm_esp32s3_procpu.conf
CONFIG_WIFI_SSID="YourNetwork"
CONFIG_WIFI_PSK="YourPassword"
```

Or use menuconfig:
```bash
cd ~/akira-workspace/AkiraOS
west build -t menuconfig
# Navigate to: Networking → WiFi
```

### Global Settings

Edit `prj.conf` for platform-agnostic settings:
```bash
# Logging level
CONFIG_LOG_DEFAULT_LEVEL=3  # 3=INFO, 4=DEBUG

# PSRAM heap size
CONFIG_HEAP_MEM_POOL_SIZE=262144  # 256KB

# Shell
CONFIG_SHELL=y
```

## Updating

### Update Zephyr & Modules

```bash
cd ~/akira-workspace
west update
west blobs fetch hal_espressif  # If using ESP32
```

### Update AkiraOS

```bash
cd ~/akira-workspace/AkiraOS
git pull origin main
git submodule update --recursive
```

### Update SDK (Rare)

Only needed when Zephyr version changes:
```bash
# Download new SDK version
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.X.Y/zephyr-sdk-0.X.Y_linux-x86_64.tar.xz
tar xvf zephyr-sdk-0.X.Y_linux-x86_64.tar.xz
cd zephyr-sdk-0.X.Y
./setup.sh

# Update environment variable
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.X.Y
```

## Troubleshooting

### "west: command not found"

```bash
pip3 install --user west
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

### "Permission denied" on /dev/ttyUSB0

```bash
sudo usermod -a -G dialout $USER
# Log out and back in
```

### "CMake Error: Zephyr SDK not found"

```bash
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.0
export ZEPHYR_BASE=~/akira-workspace/zephyr
# Add to ~/.bashrc for persistence
```

### "hal_espressif blobs not found"

```bash
cd ~/akira-workspace
west blobs fetch hal_espressif
```

### Build fails with "No rule to make target"

```bash
# Clean and rebuild
cd ~/akira-workspace/AkiraOS
rm -rf ../build
./build.sh -b <your_board>
```

## Next Steps

- [Build Your First App](first-app.md) - WASM Hello World
- [Platform Guides](../platform/) - Board-specific setup
- [Troubleshooting](troubleshooting.md) - Common issues

## Quick Reference

**Workspace:**
```bash
~/akira-workspace/
├── AkiraOS/          # Application
├── zephyr/           # Zephyr RTOS
└── build/            # Build output
```

**Common Commands:**
```bash
./build.sh -b native_sim              # Build for simulation
./build.sh -b esp32s3_devkitm_esp32s3_procpu  # Build for ESP32-S3
west flash                            # Flash to hardware
west espressif monitor                # Open serial console
```

**Build Script Options:**
```bash
./build.sh -b <board>           # Build for board
./build.sh -b <board> -r all    # Build, flash, and monitor
./build.sh -h                   # Show help
```
