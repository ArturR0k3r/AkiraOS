# ESP32-S3 Platform Guide

Complete setup and configuration guide for ESP32-S3 DevKitM.

## Hardware Overview

**ESP32-S3 DevKitM-1** is the primary development platform for AkiraOS.

### Specifications

- **CPU:** Dual-core Xtensa LX7 @ 240MHz
- **RAM:** 512KB SRAM
- **PSRAM:** 8MB (Octal SPI)
- **Flash:** 8MB (Quad SPI)
- **WiFi:** 802.11 b/g/n (2.4GHz)
- **Bluetooth:** BLE 5.0
- **USB:** USB OTG, JTAG debugging
- **GPIOs:** 45 programmable pins
- **Power:** USB-C or 5V pin

### Why ESP32-S3?

✅ **Large PSRAM** - 8MB supports multiple concurrent WASM apps  
✅ **Dual Connectivity** - WiFi + Bluetooth  
✅ **USB Support** - Native USB without external chip  
✅ **Affordable** - ~$5 USD  
✅ **Well-Supported** - Mature ESP-IDF + Zephyr integration  

---

## Getting Started

### Purchase Hardware

**Recommended:** [ESP32-S3-DevKitM-1](https://www.espressif.com/en/products/devkits/esp32-s3-devkitm-1)

**Alternative ESP32-S3 boards:**
- ESP32-S3-DevKitC-1
- ESP32-S3-WROOM-1

**Requirements:**
- 8MB PSRAM (critical for WASM apps)
- USB-C cable

### Connect Hardware

1. Connect ESP32-S3 to PC via USB-C
2. Check device appears:
   ```bash
   ls /dev/ttyUSB* # or /dev/ttyACM*
   ```
3. If no device, install CH340/CP2102 drivers

---

## Building Firmware

### Standard Build

```bash
cd ~/akira-workspace/AkiraOS
./build.sh -b esp32s3_devkitm_esp32s3_procpu
```

### Build + Flash + Monitor

```bash
./build.sh -b esp32s3_devkitm_esp32s3_procpu -r all
```

### Manual Commands

```bash
# Build only
west build -b esp32s3_devkitm_esp32s3_procpu

# Flash
west flash

# Monitor serial output
west espressif monitor
```

---

## Configuration

### WiFi Credentials

**boards/esp32s3_devkitm_esp32s3_procpu.conf:**
```bash
CONFIG_WIFI_SSID="YourNetwork"
CONFIG_WIFI_PSK="YourPassword"

# Static IP (optional)
CONFIG_NET_CONFIG_MY_IPV4_ADDR="192.168.1.100"
CONFIG_NET_CONFIG_MY_IPV4_NETMASK="255.255.255.0"
CONFIG_NET_CONFIG_MY_IPV4_GW="192.168.1.1"
```

### PSRAM Configuration

**Enabled by default:**
```bash
CONFIG_ESP_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_HEAP_MEM_POOL_SIZE=262144  # 256KB WASM heap
```

### Flash Partitions

**Default layout (8MB flash):**
```
0x0000   - 0x1000   : Bootloader
0x1000   - 0x2000   : Partition table
0x2000   - 0x5000   : NVS (settings)
0x10000  - 0x310000 : Primary slot (3MB)
0x310000 - 0x610000 : Secondary slot (3MB - OTA)
0x610000 - 0x800000 : FS partition (2MB - apps)
```

**Customize:** Edit `boards/esp32s3_devkitm_esp32s3_procpu.overlay`

---

## Features

### Peripherals

**Available on ESP32-S3:**
- ✅ I2C (sensors)
- ✅ SPI (display, SD card)
- ✅ UART (console, GPS)
- ✅ ADC (analog sensors)
- ✅ PWM (motors, LEDs)
- ✅ GPIO (digital I/O)

**Pin Configuration:** Edit device tree overlay

### Network Stack

**WiFi:**
- Station mode (connect to AP)
- AP mode (create hotspot)
- WPA2/WPA3 security

**Bluetooth:**
- BLE 5.0 (peripheral/central)
- HID device profile
- GATT services

---

## Flashing

### UART Flash (Default)

```bash
west flash --esp-device /dev/ttyUSB0
```

### USB JTAG Flash

```bash
west flash --esp-device /dev/ttyACM0 --esp-jtag-serial
```

### Erase Flash (Troubleshooting)

```bash
west espressif erase-flash
```

---

## Serial Console

### Open Monitor

```bash
west espressif monitor
```

**Baud rate:** 115200 (auto-configured)

**Exit:** `Ctrl+]`

### Alternative (picocom)

```bash
picocom -b 115200 /dev/ttyUSB0
```

---

## OTA Updates

### Via HTTP

```bash
# ESP32 must be on network
curl -X POST \
  -F "firmware=@../build/zephyr/zephyr.bin" \
  http://192.168.1.100/ota/upload
```

### Process

1. HTTP upload to secondary slot
2. Verify checksum
3. Mark pending in bootloader
4. Reboot
5. MCUboot swaps images
6. New firmware boots

---

## Power Management

### Sleep Modes

**Light Sleep:**
```c
CONFIG_PM=y
CONFIG_PM_DEVICE=y
```

**Deep Sleep (future):**
- Wake on GPIO
- Wake on timer
- Ultra-low power (<100μA)

### Power Consumption

| Mode | Current | Use Case |
|------|---------|----------|
| Active (WiFi TX) | ~160mA | File upload |
| Active (idle) | ~80mA | App running |
| Modem sleep | ~30mA | WiFi power save |
| Light sleep | ~3mA | Between tasks |
| Deep sleep | <100μA | Long standby |

---

## Debugging

### Enable Debug Output

```bash
CONFIG_LOG_DEFAULT_LEVEL=4  # DEBUG
CONFIG_AKIRA_LOG_LEVEL_DBG=y
```

### JTAG Debugging

ESP32-S3 has built-in USB JTAG:

```bash
# Launch OpenOCD
west debug

# In another terminal:
gdb-multiarch build/zephyr/zephyr.elf
(gdb) target remote :3333
(gdb) monitor reset halt
(gdb) break main
(gdb) continue
```

---

## Troubleshooting

### Can't Flash

**Error:** "Failed to connect to ESP32"

**Solutions:**
1. Press BOOT + RST buttons
2. Release RST, then BOOT
3. Try different USB cable/port
4. Check drivers installed

### WiFi Won't Connect

**Check:**
- SSID/password correct
- 2.4GHz network (5GHz not supported)
- Security type (WPA2)

**Debug:**
```bash
uart:~$ net iface
uart:~$ wifi scan
```

### OTA Timeout

**Cause:** Large firmware (>1MB) + slow network

**Workaround:**
- Use wired connection
- Reduce firmware size (`CONFIG_SIZE_OPTIMIZATIONS=y`)
- Wait for improved OTA (see IMPLEMENTATION_TASKS.md)

### PSRAM Not Detected

**Check:**
```bash
uart:~$ kernel heap
# Should show ~256KB PSRAM heap
```

**If missing:** Board may not have PSRAM chip

---

## Performance Optimization

### CPU Speed

```bash
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y
```

### Flash Speed

```bash
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
```

### PSRAM Speed

```bash
CONFIG_SPIRAM_SPEED_80M=y  # or 120M if stable
```

---

## Related Documentation

- [Platform Overview](index.md) - All platforms
- [Installation Guide](../getting-started/installation.md) - Environment setup
- [Hardware Specs](../hardware/) - Custom hardware designs
- [Troubleshooting](../getting-started/troubleshooting.md) - Common issues
