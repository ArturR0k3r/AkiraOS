# AkiraOS Quick Start Guide

## Prerequisites

- **Linux/WSL2** (Ubuntu 20.04+ recommended)
- **Python 3.8+**
- **Git**
- **West** (Zephyr's meta-tool): `pip install west`
- **Zephyr SDK** (will be installed with west)

## 🚀 First Time Setup

### Step 1: Clone Repository

```bash
# Create workspace directory (can be any name you prefer)
cd ~ && mkdir akira-workspace && cd akira-workspace

# Clone AkiraOS
git clone --recursive https://github.com/ArturR0k3r/AkiraOS.git
cd AkiraOS
```

### Step 2: Initialize West Workspace

This fetches Zephyr RTOS, WASM Micro Runtime, and all dependencies:

```bash
# Initialize west workspace
west init -l .

# Move to workspace root
cd ..

# Update all dependencies (Zephyr, WAMR, modules)
west update
```

**What `west update` does:**
- Fetches Zephyr RTOS v4.3.0
 Fetches WASM Micro Runtime from [wasm-micro-runtime]https://github.com/bytecodealliance/wasm-micro-runtime
 

west blobs fetch hal_espressif
```

### Step 6: Build and Flash

```bash
cd AkiraOS

# Build for ESP32-S3 (with MCUboot bootloader)
./build_both.sh esp32s3

# Flash to connected ESP32-S3
./flash.sh

# Monitor serial output
west espmonitor
```

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
