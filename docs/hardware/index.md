---
layout: default
title: Hardware
nav_order: 7
has_children: true
permalink: /hardware
---

# Hardware

AkiraOS custom hardware designs and specifications.

## AkiraConsole

Handheld gaming console powered by AkiraOS.

**Location:** `/docs/AkiraConsole/`

- [3D Model](../AkiraConsole/Akira.stl)
- [Schematic](../AkiraConsole/Schematic_Akira_Console.pdf)

### Specifications

- **Platform:** ESP32-S3
- **Display:** 2.8" TFT LCD
- **Input:** D-pad + 4 buttons
- **Battery:** LiPo rechargeable
- **Connectivity:** WiFi + Bluetooth

---

## AkiraMicro

Compact wearable platform.

**Location:** `/docs/AkiraMicro/`

- [3D Model](../AkiraMicro/AkiraMicro.stl)
- [Schematic](../AkiraMicro/Schematic_Akira_Micro.pdf)

### Specifications

- **Platform:** ESP32-C3
- **Display:** 1.3" OLED
- **Sensors:** IMU, temperature
- **Battery:** Coin cell
- **Form Factor:** Watch-style

---

## Supported Development Boards

See [Platform Guides](../platform/) for supported development kits:

- ESP32-S3 DevKitM
- nRF54L15 DK
- STM32 Discovery boards

---

## Custom Hardware

AkiraOS can run on custom hardware with:
- Zephyr RTOS support
- Minimum 256KB RAM
- 2MB+ flash
- Optional PSRAM for multiple apps

See [Porting Guide](../development/porting.md) for details.

---

## Schematics & Design Files

All hardware designs are open source.

**License:** [CERN Open Hardware License](https://ohwr.org/cern_ohl_s_v2.txt)

---

## Related Documentation

- [Platform Support](../platform/) - Software support for boards
- [Getting Started](../getting-started/) - Flashing firmware
