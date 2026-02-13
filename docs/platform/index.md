---
layout: default
title: Platform Support
nav_order: 5
has_children: true
permalink: /platform
---

# Platform Support

AkiraOS supports multiple embedded platforms with varying feature sets.

## Supported Platforms

| Platform | CPU | RAM | PSRAM | WiFi | BLE | Status |
|----------|-----|-----|-------|------|-----|--------|
| **[ESP32-S3](esp32-s3.md)** | Dual Xtensa @ 240MHz | 512KB | 8MB | ✅ | ✅ | ✅ Primary |
| **[Native Sim](native-sim.md)** | Host CPU | Host | N/A | ❌ | ❌ | ✅ Testing |
| **[nRF54L15](nrf54l15.md)** | Cortex-M33 @ 128MHz | 256KB | N/A | ❌ | ✅ | ✅ Supported |
| **[STM32](stm32.md)** | Cortex-M4/M7 | 256-512KB | External | ⚠️ | ⚠️ | ⚠️ Experimental |

## Quick Selection Guide

**For Production Deployment:**
- **ESP32-S3 DevKitM** - Best all-around choice
  - Large PSRAM for multiple WASM apps
  - WiFi + Bluetooth connectivity
  - USB support, affordable (~$5)

**For BLE-Only Applications:**
- **nRF54L15** - Nordic platform
  - Power-efficient BLE 5.3
  - Excellent sensor integration
  - Thread/Zigbee capable

**For Development/Testing:**
- **Native Simulation** - Fastest iteration
  - No hardware needed
  - Quick debugging with GDB
  - Ideal for algorithm development

**For STM32 Familiarity:**
- **STM32 Boards** - Experimental support
  - Leverage existing STM32 knowledge
  - Limited PSRAM (external required)
  - Best for custom hardware

## Feature Comparison

| Feature | ESP32-S3 | Native Sim | nRF54L15 | STM32 |
|---------|----------|------------|----------|-------|
| **WASM Apps** | 4 concurrent | 4 concurrent | 2-3 | 1-2 |
| **Max App Size** | 200KB | Unlimited | 100KB | 50KB |
| **OTA Updates** | ✅ WiFi | ❌ | ✅ BLE | ⚠️ UART |
| **Display Support** | ✅ SPI | ✅ SDL2 | ✅ SPI | ✅ SPI |
| **Sensors** | ✅ I2C/SPI | ⚠️ Simulated | ✅ I2C/SPI | ✅ I2C/SPI |
| **Power Modes** | Deep sleep | N/A | Ultra-low | Low power |

## Platform-Specific Guides

- [ESP32-S3 Setup Guide](esp32-s3.md) - Detailed ESP32-S3 configuration
- [Native Simulation Guide](native-sim.md) - Running on host PC
- [nRF54L15 Guide](nrf54l15.md) - Nordic platform setup
- [STM32 Guide](stm32.md) - STM32 experimental support

## Getting Started

1. Choose your platform based on the comparison above
2. Follow the platform-specific setup guide
3. Build and flash firmware
4. Deploy WASM applications

## Adding New Platforms

AkiraOS can be ported to any Zephyr-supported board. Requirements:
- Zephyr RTOS support
- Minimum 256KB RAM
- 2MB flash for firmware
- Optional: PSRAM for multiple apps

See [Porting Guide](../development/porting.md) for details.

## Related Documentation

- [Installation Guide](../getting-started/installation.md) - Environment setup
- [Architecture Overview](../architecture/) - System design
- [Hardware](../hardware/) - Custom hardware designs
