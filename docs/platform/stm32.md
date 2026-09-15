---
layout: default
title: STM32
parent: Platform Support
nav_order: 5
---

# STM32 Platform Guide

AkiraOS supports several STM32 boards with Cortex-M4/M33 cores.

## Status

**b_u585i_iot02a / steval_stwinbx1:** Supported — regularly tested  
**nucleo_l476rg:** Supported — community-tested  

## Supported Boards

| Board ID | Description | Flash | RAM |
|----------|-------------|-------|-----|
| `b_u585i_iot02a` | ST B-U585I-IOT02A Discovery Kit (STM32U585) | 2MB | 786KB |
| `steval_stwinbx1` | ST STEVAL-STWINBX1 (STM32U585) | 2MB | 786KB |
| `nucleo_l476rg` | ST Nucleo L476RG (STM32L476RG) | 1MB | 96KB |

## Features

- Cortex-M4/M33 ARM architecture
- Flash partitioning for MCUboot + dual OTA slots
- LittleFS internal flash storage
- I2C, SPI, ADC, UART, timers
- WiFi/BT via external modules (b_u585i_iot02a / steval_stwinbx1)

## Getting Started

### Build

```bash
# STM32U5 IoT Discovery Kit
./build.sh -b b_u585i_iot02a

# ST STEVAL-STWINBX1
./build.sh -b steval_stwinbx1

# Nucleo L476RG (constrained — 96KB RAM)
./build.sh -b nucleo_l476rg
```

### Build with MCUboot bootloader

```bash
./build.sh -b b_u585i_iot02a -bl y
```

### Flash (ST-Link)

```bash
./build.sh -b b_u585i_iot02a -r a
# or manually:
west flash
```

---

## Configuration Notes

### b_u585i_iot02a / steval_stwinbx1
Full feature set — networking, BT, OTA, LittleFS + external flash, USB HID.  
See [boards/b_u585i_iot02a.conf](https://github.com/ArturR0k3r/AkiraOS/blob/main/boards/b_u585i_iot02a.conf) and  
[boards/steval_stwinbx1.conf](https://github.com/ArturR0k3r/AkiraOS/blob/main/boards/steval_stwinbx1.conf).

### nucleo_l476rg
Constrained board (96KB SRAM). Networking buffers, FAT filesystem, and WebSocket  
are disabled in the board config to fit RAM. WASM heap and app slots are reduced.  
LittleFS is available on internal flash.  
See [boards/nucleo_l476rg.conf](https://github.com/ArturR0k3r/AkiraOS/blob/main/boards/nucleo_l476rg.conf).

---

## Limitations

- Nucleo L476RG: no onboard WiFi/BT — OTA requires USB/serial
- Nucleo L476RG: max 1 running WASM app, 4 installed apps (RAM constraint)

---

## Related Documentation

- [Platform Overview](index.md)
- [Building Apps](../development/building-apps.md)
- [Development Guide](../development/index.md)

---

*Last updated: 2026-09-14 (AkiraOS v1.6.5)*
