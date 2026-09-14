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

`build.sh` discovers boards automatically from the metadata headers in `boards/*.conf`,
so **`./build.sh -h` is the authoritative list**. There are currently **17 selectable
boards**. Pass either the canonical name or an alias to `-b`.

### Boards with a written guide

| Platform | CPU | RAM | PSRAM | WiFi | BLE | Status |
|----------|-----|-----|-------|------|-----|--------|
| [ESP32-S3](esp32-s3.md) | Dual Xtensa LX7 @ 240 MHz | 512 KB | 8 MB | Yes | Yes | Primary |
| [ESP32-C3](esp32-c3.md) | RISC-V @ 160 MHz | 400 KB | No | Yes | No (default) | Supported |
| [nRF54L15](nrf54l15.md) | Cortex-M33 @ 128 MHz | 256 KB | No | No | Yes | Supported |
| [STM32](stm32.md) | Cortex-M4/M7 | 256–512 KB | External | Partial | Partial | Experimental |
| [Native Sim](native-sim.md) | Host CPU | Host | N/A | No | No | Testing only |

### All build targets

`Flash` is whether `./build.sh -r a` can flash the board directly. Targets marked **no**
build correctly but must be flashed with an external tool.

| `-b` name | Alias | Zephyr board | Chip | Flash | Description |
|-----------|-------|--------------|------|-------|-------------|
| `native_sim` | — | `native_sim` | native | n/a | Native Simulator (default) |
| `akiraconsole_esp32s3_procpu` | `akiraconsole` | `akiraconsole/esp32s3/procpu` | esp32s3 | yes | Akira Console (ESP32-S3 N16R8) |
| `akiraconsole_prod_esp32s3_procpu` | `akiraconsole_prod` | `akiraconsole_prod/esp32s3/procpu` | esp32s3 | yes | AkiraConsole Production (ESP32-S3-WROOM-1-N16R8) |
| `esp32s3_devkitm_esp32s3_procpu` | — | `esp32s3_devkitm/esp32s3/procpu` | esp32s3 | yes | ESP32-S3 DevKitM |
| `esp32s3_super_mini_esp32s3_procpu` | `esp32s3_super_mini` | `esp32s3_super_mini/esp32s3/procpu` | esp32s3 | yes | ESP32-S3 Super Mini (4 MB flash, 2 MB quad PSRAM) |
| `esp32_devkitc_procpu` | — | `esp32_devkitc/esp32/procpu` | esp32 | yes | ESP32 DevKitC |
| `esp32c3_devkitm` | — | `esp32c3_devkitm` | esp32c3 | yes | ESP32-C3 DevKitM (RISC-V) |
| `esp32c6_devkitc_esp32c6_hpcore` | — | `esp32c6_devkitc/esp32c6/hpcore` | esp32c6 | **no** | ESP32-C6 DevKitC (RISC-V HP core) |
| `esp32h2_devkitm_esp32h2` | — | `esp32h2_devkitm/esp32h2` | esp32h2 | **no** | ESP32-H2 DevKitM (RISC-V) |
| `xiao_esp32c6_esp32c6_hpcore` | — | `xiao_esp32c6/esp32c6/hpcore` | esp32c6 | **no** | Seeed XIAO ESP32-C6 (4 MB flash) |
| `nrf54l15dk_nrf54l15_cpuapp` | — | `nrf54l15dk/nrf54l15/cpuapp` | nrf54l15 | yes | Nordic nRF54L15 DK |
| `b_u585i_iot02a` | — | `b_u585i_iot02a` | stm32 | yes | STM32U5 IoT Discovery Kit |
| `nucleo_l476rg` | — | `nucleo_l476rg` | stm32 | yes | ST Nucleo L476RG |
| `steval_stwinbx1` | — | `steval_stwinbx1` | stm32 | yes | ST STEVAL-STWINBX1 |
| `nucleo_h743zi` | `h743`, `stm32h7` | `nucleo_h743zi` | stm32h743zi | **no** | ST Nucleo-H743ZI (Cortex-M7 @ 480 MHz) |
| `rpi_pico` | `pico`, `rp2040` | `rpi_pico` | rp2040 | **no** | Raspberry Pi Pico (RP2040) |
| `rpi_pico2_rp2350a_m33` | `pico2`, `rp2350` | `rpi_pico2/rp2350a/m33` | rp2350 | **no** | Raspberry Pi Pico 2 (RP2350, Cortex-M33) |

`boards/template/` is a scaffold for new ports, not a selectable target — see the
[Porting Guide](../hardware/porting-guide.md).

## Quick Selection Guide

**For Production Deployment:**
- **ESP32-S3** — Best all-around choice. Large PSRAM for multiple WASM apps, WiFi + Bluetooth connectivity, USB support.

**For BLE-Only Applications:**
- **nRF54L15** — Power-efficient BLE 5.3, excellent sensor integration, Thread/Zigbee capable.

**For Development and Testing:**
- **Native Simulation** — No hardware needed. Quick debugging with GDB. Ideal for algorithm development.

**For STM32 Familiarity:**
- **STM32 Boards** — Experimental support. Limited without external PSRAM.

## Feature Comparison

| Feature | ESP32-S3 | Native Sim | nRF54L15 | STM32 |
|---------|----------|------------|----------|-------|
| Max concurrent WASM apps | 2 | 2 | 1–2 | 1 |
| Max app size | 200 KB | Unlimited | 100 KB | 50 KB |
| OTA updates | Yes (WiFi) | No | Yes (BLE) | Partial (UART) |
| Display support | Yes (SPI) | Yes (SDL2) | Yes (SPI) | Yes (SPI) |
| Sensor support | Yes (I2C/SPI) | Simulated | Yes (I2C/SPI) | Yes (I2C/SPI) |
| Power modes | Deep sleep | N/A | Ultra-low power | Low power |

## Platform-Specific Guides

- [ESP32-S3 Setup Guide](esp32-s3.md) — Detailed ESP32-S3 configuration
- [ESP32-C3 Setup Guide](esp32-c3.md) — RISC-V variant
- [Native Simulation Guide](native-sim.md) — Running on host PC
- [nRF54L15 Guide](nrf54l15.md) — Nordic platform setup
- [STM32 Guide](stm32.md) — STM32 experimental support

## Getting Started

1. Choose your platform from the comparison table above
2. Follow the platform-specific setup guide
3. Build and flash firmware
4. Deploy WASM applications

## Adding New Platforms

AkiraOS can be ported to any Zephyr-supported board. Requirements:
- Zephyr RTOS support
- Minimum 256 KB RAM
- 2 MB flash for firmware
- Optional: PSRAM for multiple concurrent apps

## Related Documentation

- [Installation Guide](../getting-started/installation.md) — Environment setup
- [Architecture Overview](../architecture) — System design
- [Hardware](../hardware) — Custom hardware designs

---

*Last updated: 2026-09-14 (AkiraOS v1.6.4)*
