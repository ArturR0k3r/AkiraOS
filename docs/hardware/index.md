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

Product firmware can live in its own repository and use AkiraOS as a Zephyr
module, without forking:

```yaml
# your-product/west.yml
manifest:
  projects:
    - name: akira-os
      url: https://github.com/ArturR0k3r/AkiraOS.git
      revision: v1.6.x      # pin a release tag once one is cut
      path: akira-os
      import: true          # also brings in Zephyr, WAMR and TFLite Micro
  self:
    path: your-product
```

In the product application, enable AkiraOS and link its interface target:

```cmake
# your-product/CMakeLists.txt
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(your_product)

target_sources(app PRIVATE src/main.c)
target_link_libraries(app PRIVATE akira_os)   # AkiraOS include paths
```

```kconfig
# your-product/prj.conf
CONFIG_AKIRA_OS=y   # every CONFIG_AKIRA_* option depends on this
```

`main()` can return `akira_start()` to run the standard AkiraOS boot sequence.
Board definitions and DTS bindings from `akira-os/boards` are found
automatically. The [out-of-tree product sample](https://github.com/ArturR0k3r/AkiraOS/tree/v1.6.x/samples/out_of_tree_product)
is a complete example that CI builds on every change.

---

## Schematics & Design Files

Aki hardware designs are open source.

**License:** [CERN Open Hardware License](https://ohwr.org/cern_ohl_s_v2.txt)

---

## Related Documentation

- [Platform Support](../platform) - Software support for boards
- [Getting Started](../getting-started) - Flashing firmware

---

*Last updated: 2026-09-14 (AkiraOS v1.6.4)*
