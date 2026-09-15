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


- [SD Card Setup](sd-card.md)

### Specifications

- **Platform:** ESP32-S3
- **Display:** 2.8" TFT LCD 
- **Input:** D-pad + 4 buttons
- **Storage:** microSD card slot (FAT32, up to 32 GB)
- **Battery:** LiPo rechargeable with fuel gauge and bettery control accesible fromm software
- **Connectivity:** WiFi + Bluetooth + LoRa 2.4 and 868/915 + SubGHz + NFC 

---

## AkiraMicro

Compact wearable platform.

**Coming soon...:** `/docs/AkiraMicro/`

### Specifications

- **Platform:** ESP32-C6
- **Display:** 1.3" OLED
- **Sensors:** ???
- **Battery:** ???
- **Form Factor:** ???

---

## Supported Development Boards

See [Platform Guides](../platform) for supported development kits:

- ESP32 boards
- nRF boards
- STM32 boards

---

## Custom Hardware

AkiraOS can run on custom hardware. OEM porting resources:

- [**Porting Guide**](porting-guide.md) — end-to-end walkthrough: BSP files, display, WASM runtime, OTA, and a day-by-day week plan to first running app.
- [**BSP Template Scaffold**](https://github.com/ArturR0k3r/AkiraOS/tree/main/boards/template) — copy-and-rename starting point with FIXME markers for GPIO, SPI, I2C, PSRAM, display, and flash partitions.

### Using AkiraOS as a west module

Product firmware lives in its own repository and uses AkiraOS as a Zephyr
module — no fork. Pin an AkiraOS tag in your `west.yml`, pick a service profile,
add your board, and register capabilities, native APIs and hooks from your own
code. See the full walkthrough:

- [**Using AkiraOS as a west module**](west-module.md) — new product repo to a
  booting image, profiles, custom boards, and the extension points.

## Schematics & Design Files

Aki hardware designs are open source.

**License:** [CERN Open Hardware License](https://ohwr.org/cern_ohl_s_v2.txt)

---

## Related Documentation

- [Platform Support](../platform) - Software support for boards
- [Getting Started](../getting-started) - Flashing firmware

---

*Last updated: 2026-09-14 (AkiraOS v1.6.5)*
