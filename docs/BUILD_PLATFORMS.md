# AkiraOS Build Platforms

This document describes the supported hardware platforms for AkiraOS and their intended use cases.

## Platform Overview

| Platform | Architecture | Use Case | Status |
|----------|--------------|----------|--------|
| ESP32-S3 DevKit M | Xtensa LX7 dual-core | **Akira Console** (Primary) | ✅ Fully Supported |
| ESP32 DevKitC | Xtensa LX6 dual-core | Akira Console (Legacy) | ✅ Supported |
| ESP32-C3 DevKit M | RISC-V 32-bit | **Akira Modules Only** | ✅ Supported |
| Native Sim | x86_64 | Testing/Development | ✅ Supported |

## Platform Details

### ESP32-S3 DevKit M (Primary Target)

**Architecture:** Xtensa LX7 dual-core @ 240MHz  
**Memory:** 512KB SRAM, 8MB PSRAM, 8MB Flash  
**Use Case:** **Akira Console** - Main handheld device  

**Features:**
- Full Akira Console functionality
- ILI9341 TFT display support
- WiFi networking
- SD card support (SDMMC)
- OTA updates
- Complete Akira Module System
- All sensor drivers (NRF24L01, LSM6DS3, INA219)
- Audio capabilities
- USB support

**Memory Usage:**
- FLASH: 8.84% (plenty of room for features)
- RAM: 77.38% (good headroom)

**Status:** ✅ Production Ready

---

### ESP32 DevKitC (Legacy Support)

**Architecture:** Xtensa LX6 dual-core @ 240MHz  
**Memory:** 520KB SRAM, 4MB Flash  
**Use Case:** Akira Console (Legacy/Budget)  

**Features:**
- Akira Console functionality (limited)
- WiFi networking
- OTA updates
- Basic display support
- Limited module support

**Memory Usage:**
- FLASH: 18.36%
- RAM: 95.33% ⚠️ **Very High**

**Status:** ✅ Supported (with limitations)

**Note:** High RAM usage (95.33%) limits feature expansion. Consider ESP32-S3 for new designs.

---

### ESP32-C3 DevKit M (Modules Only)

**Architecture:** RISC-V 32-bit single-core @ 160MHz  
**Memory:** 400KB SRAM, 4MB Flash  
**Use Case:** 🔧 **Akira Modules Only** - External sensor/control nodes  

**Important:** This platform is **NOT** intended for the Akira Console. It is designed exclusively for:

- Remote sensor nodes
- Wireless peripheral modules
- I2C/SPI sensor interfaces
- GPIO control modules
- Low-power edge devices
- Distributed system nodes

**Why Not for Akira Console?**
- Single-core processor (vs dual-core)
- Limited SRAM (400KB vs 512KB)
- No PSRAM support
- Reduced peripheral set
- Display support limited/simplified

**Akira Module Use Cases:**
1. **Remote Sensor Node**
   - LSM6DS3 IMU + NRF24L01 wireless
   - Read sensors, transmit data to console
   - Low power operation

2. **I2C Peripheral Hub**
   - Multiple I2C sensors/devices
   - WiFi connectivity to console
   - Simple data aggregation

3. **GPIO Expander**
   - Control relays, LEDs, motors
   - Networked control via Akira Module API
   - Event-driven operation

4. **Wireless Bridge**
   - NRF24L01 to WiFi gateway
   - Protocol translation
   - Range extension

**Memory Usage:**
- FLASH: 19.27%
- RAM: 81.75%

**Status:** ✅ Supported for Akira Modules

**Example Configuration:**
```c
// ESP32-C3 as remote sensor module
CONFIG_AKIRA_MODULE=y
CONFIG_AKIRA_MODULE_LSM6DS3=y
CONFIG_AKIRA_MODULE_NRF24L01=y
CONFIG_AKIRA_MODULE_COMM_NETWORK=y

// Disable console-specific features
CONFIG_DISPLAY=n
CONFIG_LVGL=n
CONFIG_SHELL=n  // Optional, can keep for debug
```

---

### Native Sim (Development)

**Architecture:** x86_64 (host system)  
**Use Case:** Testing, simulation, development  

**Features:**
- Full Zephyr RTOS simulation
- File system testing
- Network stack testing
- Module system development
- No hardware required

**Status:** ✅ Development Tool

---

## Build Target Selection

### For Akira Console Development
Use ESP32-S3 (primary) or ESP32 (legacy):
```bash
# ESP32-S3 (recommended)
west build -b esp32s3_devkitm/esp32s3/procpu -p

# ESP32 (legacy, limited RAM)
west build -b esp32_devkitc/esp32/procpu -p
```

### For Akira Module Development
Use ESP32-C3:
```bash
# ESP32-C3 for remote modules
west build -b esp32c3_devkitm -p
```

### For Testing/Simulation
Use Native Sim:
```bash
# Native simulator
west build -b native_sim -p
./build/zephyr/zephyr.exe
```

---

## Memory Comparison

| Platform | FLASH Available | RAM Available | PSRAM | Best For |
|----------|----------------|---------------|-------|----------|
| ESP32-S3 | 8MB | 512KB | 8MB | Akira Console |
| ESP32 | 4MB | 520KB | No | Legacy Console |
| ESP32-C3 | 4MB | 400KB | No | Akira Modules |
| Native Sim | Host | Host | Host | Development |

---

## Recommended Platform Usage

### ✅ Use ESP32-S3 for:
- Akira Console (handheld device)
- Full-featured applications
- Display-intensive apps
- Multi-module orchestration
- Audio/video processing
- Maximum flexibility

### ✅ Use ESP32 for:
- Budget Akira Console builds
- Legacy hardware support
- Simple console applications
- When ESP32-S3 unavailable

### ✅ Use ESP32-C3 for:
- **Akira Modules only**
- Remote sensor nodes
- Wireless peripherals
- I2C/SPI interfaces
- Distributed sensors
- Low-cost edge devices
- Battery-powered modules

### ✅ Use Native Sim for:
- Unit testing
- Algorithm development
- CI/CD pipelines
- Cross-platform testing
- No-hardware development

---

## Feature Matrix

| Feature | ESP32-S3 | ESP32 | ESP32-C3 | Native Sim |
|---------|----------|-------|----------|------------|
| Akira Console | ✅ Primary | ✅ Legacy | ❌ No | ⚠️ Limited |
| Akira Modules | ✅ Yes | ✅ Yes | ✅ **Primary** | ✅ Yes |
| Display (ILI9341) | ✅ Full | ✅ Full | ⚠️ Basic | ❌ No |
| WiFi | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Simulated |
| SD Card | ✅ SDMMC | ✅ SPI | ✅ SPI | ✅ Simulated |
| OTA Updates | ✅ Yes | ✅ Yes | ✅ Yes | ❌ No |
| USB | ✅ Yes | ⚠️ Limited | ✅ Yes | ❌ No |
| Dual Core | ✅ Yes | ✅ Yes | ❌ No | ⚠️ Multi-thread |
| PSRAM | ✅ 8MB | ❌ No | ❌ No | N/A |
| Power Efficiency | Good | Good | ✅ **Best** | N/A |

---

## Architecture Selection Guide

```
┌─────────────────────────────────────┐
│     What are you building?          │
└─────────────────────────────────────┘
                 │
                 ├─ Akira Console (handheld device)?
                 │  └─> ESP32-S3 (or ESP32 for legacy)
                 │
                 ├─ Remote sensor node?
                 │  └─> ESP32-C3
                 │
                 ├─ Wireless peripheral?
                 │  └─> ESP32-C3
                 │
                 ├─ Low-power edge device?
                 │  └─> ESP32-C3
                 │
                 └─ Testing/Development?
                    └─> Native Sim
```

---

## Build All Platforms

Test script for building all platforms:

```bash
#!/bin/bash
# Build all platforms

echo "Building for all platforms..."

# Native sim
west build -b native_sim -p
echo "✅ Native Sim built"

# ESP32-S3 (Akira Console)
west build -b esp32s3_devkitm/esp32s3/procpu -p
echo "✅ ESP32-S3 built (Akira Console)"

# ESP32 (Legacy Console)
west build -b esp32_devkitc/esp32/procpu -p
echo "✅ ESP32 built (Legacy Console)"

# ESP32-C3 (Akira Modules)
west build -b esp32c3_devkitm -p
echo "✅ ESP32-C3 built (Akira Modules)"

echo ""
echo "All platforms built successfully!"
echo "ESP32-S3: Primary Akira Console"
echo "ESP32:    Legacy Akira Console"
echo "ESP32-C3: Akira Modules ONLY"
echo "Native:   Testing/Development"
```

---

## Summary

- **Akira Console** → ESP32-S3 (primary) or ESP32 (legacy)
- **Akira Modules** → ESP32-C3 (optimized for remote nodes)
- **Development** → Native Sim

**Important:** ESP32-C3 is specifically designed for Akira Modules (remote sensor/control nodes) and should **not** be used for the Akira Console handheld device.

---

Last Updated: November 10, 2025  
Version: 1.0.0
