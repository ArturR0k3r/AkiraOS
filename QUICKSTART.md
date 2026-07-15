# AkiraOS Quick Start Guide

<div align="center">

<img src="assets/logo.png" alt="AkiraOS Logo" width="250"/>

**Get AkiraOS running in under 15 minutes**

</div>

---

## Prerequisites

| Requirement | Version | Install |
|-------------|---------|---------|
| **Linux / WSL2** | Ubuntu 20.04+ | [WSL setup](https://docs.microsoft.com/windows/wsl/install) |
| **Python** | 3.8+ | `sudo apt install python3 python3-pip` |
| **Git** | 2.25+ | `sudo apt install git` |
| **CMake** | 3.20+ | `sudo apt install cmake` |
| **Device Tree Compiler** | 1.4+ | `sudo apt install device-tree-compiler` |
| **West** | Latest | `pip3 install west` |

Install all required packages at once:

```bash
sudo apt update
sudo apt install --no-install-recommends git cmake ninja-build gperf \
  ccache dfu-util device-tree-compiler wget \
  python3-dev python3-pip python3-setuptools python3-tk python3-wheel \
  xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1

pip3 install --user -U west pyelftools

# Add Python user bin to PATH (add to ~/.bashrc for persistence)
export PATH="$HOME/.local/bin:$PATH"
```

---

## Step 1: Create Workspace

AkiraOS uses a **West workspace** where Zephyr and other dependencies live alongside your project.

```bash
mkdir ~/akira-workspace && cd ~/akira-workspace
git clone --recursive https://github.com/ArturR0k3r/AkiraOS.git
cd AkiraOS
```

---

## Step 2: Initialize West

West fetches Zephyr RTOS, MCUboot, HAL drivers, and all required modules (~500 MB).

```bash
# From inside AkiraOS/
west init -l .
cd ..         # move to workspace root
west update   # fetch all dependencies (~5–10 min)
```

After `west update`, your workspace looks like:

```
~/akira-workspace/
├── AkiraOS/          # your application (this repo)
├── zephyr/           # Zephyr RTOS
├── bootloader/       # MCUboot
├── modules/          # Zephyr modules
└── tools/            # build utilities
```

---

## Step 3: Install Zephyr SDK

The Zephyr SDK provides cross-compilers for all supported architectures.

```bash
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.4/zephyr-sdk-0.17.4_linux-x86_64.tar.xz
tar xvf zephyr-sdk-0.17.4_linux-x86_64.tar.xz
cd zephyr-sdk-0.17.4
./setup.sh
```

---

## Step 4: Fetch ESP32 Blobs (ESP32 targets only)

Required binary blobs for ESP32 WiFi/BLE firmware:

```bash
cd ~/akira-workspace
west blobs fetch hal_espressif
```

Skip this step if you are only using `native_sim`.

---

## Step 5: Build and Test (Native Simulation)

Native simulation runs AkiraOS on your host CPU — no hardware required. This is the recommended starting point.

```bash
cd ~/akira-workspace/AkiraOS
./build.sh          # builds native_sim and runs it automatically
```

The executable is at `../build-native-sim/zephyr/zephyr.exe` and is launched automatically after a successful build.

**Advantages of native_sim:**
- No flashing — instant feedback
- GDB debugging support
- Tests core logic without hardware

**Limitations:**
- No real WiFi / Bluetooth / peripherals
- Simulated timing only

---

## Step 6: Build for Hardware

Recommended board: **ESP32-S3 DevKitM** (full feature support).

```bash
cd ~/akira-workspace/AkiraOS

# Build for ESP32-S3 (primary target)
./build.sh -b esp32s3_devkitm_esp32s3_procpu

# Or other supported boards:
./build.sh -b esp32_devkitc_procpu    # ESP32 classic
./build.sh -b esp32c3_devkitm        # ESP32-C3 (RISC-V)
```

Run `./build.sh -h` to see all available boards.

Build output is placed at the workspace root:

```
~/akira-workspace/
├── build-esp32s3-devkitm-esp32s3-procpu/
│   └── zephyr/
│       ├── zephyr.elf      # ELF binary
│       ├── zephyr.bin      # raw firmware
│       └── zephyr.hex      # Intel HEX
└── build-native-sim/
    └── zephyr/
        └── zephyr.exe
```

---

## Step 7: Flash to Hardware

Connect your ESP32 board via USB, then:

```bash
# Build and flash application
./build.sh -b esp32s3_devkitm_esp32s3_procpu -r a

# Build, flash bootloader + application (first time or after MCUboot changes)
./build.sh -b esp32s3_devkitm_esp32s3_procpu -bl y -r all
```

If the flash fails, check the serial port:

```bash
ls /dev/ttyUSB* /dev/ttyACM*
```

If you see "Permission denied", add yourself to the `dialout` group:

```bash
sudo usermod -a -G dialout $USER
# Log out and back in for the change to take effect
```

---

## Step 8: Verify Boot

Monitor serial output:

```bash
west espmonitor              # ESP32 (recommended)
# or:
picocom -b 115200 /dev/ttyUSB0
```

Expected boot output:

```
*** Booting Zephyr OS build v4.3.0 ***
[00:00:00.123] <inf> main: AkiraOS v1.5.8 starting...
[00:00:00.234] <inf> runtime: WAMR initialized
[00:00:00.345] <inf> connectivity: WiFi stack ready
[00:00:00.456] <inf> main: System initialized successfully

AkiraOS:~$
```

Press **Tab** to see available shell commands.

---

## Next Steps

### Explore the shell

```bash
AkiraOS:~$ kernel version   # Zephyr version
AkiraOS:~$ kernel uptime    # uptime
AkiraOS:~$ wasm status      # running WASM apps
AkiraOS:~$ net iface        # network status
AkiraOS:~$ fs ls /          # file system
AkiraOS:~$ help             # full command list
```

### Upload a WASM application

```bash
# Build a WASM app (requires WASI SDK — see AkiraSDK/README.md)
cd ~/akira-workspace/AkiraOS/AkiraSDK/wasm_apps
./build.sh hello_world
# produces bin/hello_world.wasm

# Install to device over HTTP (device must be on WiFi).
# App binary is streamed raw to the app-manager route; /upload is firmware only.
curl -X POST --data-binary @bin/hello_world.wasm \
  "http://<device-ip>/api/apps/install?name=hello_world"
```

### Perform an OTA firmware update

```bash
cd ~/akira-workspace/AkiraOS
./build.sh -b esp32s3_devkitm_esp32s3_procpu  # build new firmware
curl -X POST -F "firmware=@../build-esp32s3-devkitm-esp32s3-procpu/zephyr/zephyr.bin" \
  http://<device-ip>/ota/upload
```

---

## Platform Comparison

| Platform | CPU | RAM | PSRAM | WiFi | BLE | USB | Best for |
|----------|-----|-----|-------|------|-----|-----|----------|
| **ESP32-S3** | 2× Xtensa @ 240 MHz | 512 KB | 8 MB | Yes | Yes | Yes | **Primary target** |
| **ESP32** | 2× Xtensa @ 240 MHz | 520 KB | Limited | Yes | Yes | No | Legacy |
| **ESP32-C3** | RISC-V @ 160 MHz | 400 KB | No | Yes | No (see note) | Yes | Low-cost |
| **native_sim** | Host CPU | Host | N/A | No | No | No | Development |

> **ESP32-C3 note:** BLE is disabled by default (`CONFIG_BT=n`) because WiFi and BLE cannot run simultaneously within the 32KB heap budget. To enable BLE, disable WiFi and reduce WASM app memory quotas accordingly.

---

## Configuration

### Global settings (`prj.conf`)

Applied to all platforms:

```
CONFIG_LOG_DEFAULT_LEVEL=3     # 3 = INFO (default); set to 4 for DEBUG
CONFIG_HEAP_MEM_POOL_SIZE=65536   # 64 KB kernel heap (global default)
CONFIG_WAMR_AOT_SUPPORT=n      # AOT disabled globally; enabled per-board (e.g. akiraconsole)
```

### Board-specific settings (`boards/<board>.conf`)

Override or extend global config per board:

```
CONFIG_NET_SOCKETS_SERVICE_STACK_SIZE=8192
```

### Interactive configuration

```bash
cd ~/akira-workspace/AkiraOS
west build -t menuconfig   # TUI config browser
```

### WiFi credentials

Set in `prj.conf` or the board-specific `.conf`:

```
CONFIG_WIFI_SSID="YourNetwork"
CONFIG_WIFI_PSK="YourPassword"
```

---

## Troubleshooting

### "west: command not found"

```bash
pip3 install --user west
export PATH="$HOME/.local/bin:$PATH"  # add to ~/.bashrc for persistence
west --version  # verify
```

### "cmake: command not found"

```bash
sudo apt update && sudo apt install cmake ninja-build
```

### "Permission denied" when flashing

```bash
sudo usermod -a -G dialout $USER
# Log out and back in, then retry
```

### "No module named 'elftools'"

```bash
pip3 install pyelftools
```

### ESP32 flash fails with "Failed to connect"

1. Verify serial port: `ls /dev/ttyUSB* /dev/ttyACM*`
2. Manually enter download mode: hold **BOOT**, press **RESET**, release **BOOT**
3. Flash with explicit port: `west flash --port /dev/ttyUSB0`
4. Check USB cable (must carry data, not only power)

### "west update" fails with network errors

```bash
west update -v   # retry with verbose output
```

### Build fails with submodule errors

```bash
cd ~/akira-workspace/AkiraOS
git submodule update --init --recursive --force
git submodule status  # verify
```

### "ZEPHYR_BASE not set" error

```bash
cd ~/akira-workspace/AkiraOS
west init -l .
cd ..
west update
```

### Out of memory during build

```bash
# Limit parallel jobs
MAKEFLAGS="-j2" ./build.sh -b esp32s3_devkitm_esp32s3_procpu
```

### native_sim crashes on startup

```bash
sudo apt install libsdl2-dev
./build.sh -c && ./build.sh   # clean and rebuild
```

### Getting more help

1. Enable verbose output: `west -v build`
2. Check build logs in the build directory
3. Search [GitHub Issues](https://github.com/ArturR0k3r/AkiraOS/issues)
4. Ask in [GitHub Discussions](https://github.com/ArturR0k3r/AkiraOS/discussions)

When reporting issues, include:
- OS and version (`uname -a`)
- West version (`west --version`)
- Full build command and output
- Board type

---

## Dive Deeper

| Topic | Resource |
|-------|----------|
| WASM app development | [AkiraSDK/README.md](AkiraSDK/README.md) |
| Full API reference | [AkiraSDK/docs/API_REFERENCE.md](AkiraSDK/docs/API_REFERENCE.md) |
| Architecture | [docs/architecture/system-overview.md](docs/architecture/system-overview.md) |
| Contributing | [CONTRIBUTING.md](CONTRIBUTING.md) |

---

<div align="center">

**You're ready to develop with AkiraOS!**

Questions? Open an issue on [GitHub](https://github.com/ArturR0k3r/AkiraOS/issues)

[Back to Top](#akiraos-quick-start-guide)

</div>
