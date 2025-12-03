# AkiraOS Supported Platforms

This document lists all supported hardware platforms for AkiraOS.

## Quick Reference

| Board ID | Zephyr Board | Description | Features |
|----------|--------------|-------------|----------|
| `native_sim` | native_sim | Native Simulator | Testing/Development |
| `esp32s3_devkitm_esp32s3_procpu` | esp32s3_devkitm/esp32s3/procpu | ESP32-S3 DevKitM | WiFi, BLE, Display |
| `esp32_devkitc_procpu` | esp32_devkitc/esp32/procpu | ESP32 DevKitC | WiFi, BLE |
| `xiao_nrf54l15_nrf54l15_cpuapp` | xiao_nrf54l15/nrf54l15/cpuapp | Seeed XIAO nRF54L15 | BLE 6.0, 802.15.4 |
| `steval_stwinbx1` | steval_stwinbx1 | ST STEVAL-STWINBX1 | BLE, WiFi, Sensors |

---

## ESP32-S3 DevKitM (Primary)

**Board ID:** `esp32s3_devkitm_esp32s3_procpu`

### Specifications
- **SoC:** ESP32-S3 (Xtensa LX7 dual-core @ 240MHz)
- **Flash:** 8MB
- **PSRAM:** 8MB
- **WiFi:** 802.11 b/g/n
- **Bluetooth:** BLE 5.0

### Supported Features
- ✅ WiFi Station/AP
- ✅ Bluetooth LE
- ✅ SPI Display (ILI9341)
- ✅ I2C Sensors
- ✅ MCUboot (OTA)
- ✅ NVS Settings
- ✅ Web Server
- ✅ Shell (UART/Web)

### Build & Flash
```bash
./build.sh -b esp32s3_devkitm_esp32s3_procpu -bl y -r all
```

---

## Seeed XIAO nRF54L15

**Board ID:** `xiao_nrf54l15_nrf54l15_cpuapp`

### Specifications
- **SoC:** Nordic nRF54L15
- **CPU:** ARM Cortex-M33 @ 128MHz + RISC-V coprocessor
- **Flash:** Up to 1.5MB NVM
- **RAM:** Up to 256KB
- **Radio:** 2.4GHz multiprotocol

### Supported Features
- ✅ Bluetooth 6.0 (including Channel Sounding)
- ✅ IEEE 802.15.4 (Thread, Zigbee, Matter)
- ✅ Ultra-low power (300nA standby)
- ✅ TrustZone security
- ✅ USB CDC (built-in)
- ✅ I2C/SPI Sensors
- ✅ PWM LEDs
- ✅ ADC

### Build & Flash
```bash
./build.sh -b xiao_nrf54l15_nrf54l15_cpuapp -r a
```

### Programming
The XIAO nRF54L15 has built-in CMSIS-DAP via SAMD11, so no external programmer is needed. Just connect USB and run `west flash`.

---

## ST STEVAL-STWINBX1 (SensorTile Wireless Industrial Node)

**Board ID:** `steval_stwinbx1`

### Specifications
- **SoC:** STM32U585AI (ARM Cortex-M33 @ 160MHz)
- **Flash:** 2MB
- **RAM:** 786KB
- **Connectivity:** BLE 5.2 (BlueNRG-M2SA), WiFi (EMW3080), NFC

### On-Board Sensors
| Sensor | Type | Interface |
|--------|------|-----------|
| ISM330DHCX | 6-axis IMU + ML Core | SPI |
| IIS2DLPC | 3-axis Accelerometer | SPI |
| IIS2ICLX | 2-axis Inclinometer + ML Core | SPI |
| STTS22H | Temperature | I2C |
| IIS2MDC | 3-axis Magnetometer | I2C |
| ILPS22QS | Pressure | I2C |
| IMP23ABSU | Analog Microphone | Analog |
| IMP34DT05 | Digital Microphone | PDM |

### Supported Features
- ✅ Bluetooth LE 5.2
- ✅ WiFi (MXCHIP EMW3080)
- ✅ NFC (ST25DV64K)
- ✅ Multiple Sensors
- ✅ USB Type-C
- ✅ microSD Card
- ✅ CAN FD
- ✅ RS485

### Build & Flash
```bash
# Flash via DFU (hold USER button while connecting USB)
./build.sh -b steval_stwinbx1 -r a

# Or with ST-LINK
west flash -d build-steval_stwinbx1
```

### Programming Methods
1. **DFU Mode:** Hold USER button, connect USB-C
2. **SWD:** Use ST-LINK/V3 via STDC14 connector (CN4)

---

## ESP32 DevKitC (Legacy)

**Board ID:** `esp32_devkitc_procpu`

### Specifications
- **SoC:** ESP32 (Xtensa LX6 dual-core @ 240MHz)
- **Flash:** 4MB
- **WiFi:** 802.11 b/g/n
- **Bluetooth:** Classic + BLE 4.2

### Build & Flash
```bash
./build.sh -b esp32_devkitc_procpu -r a
```

---

## Native Simulator

**Board ID:** `native_sim`

For testing and development on host PC.

### Build & Run
```bash
./build.sh                    # Build
./build.sh -b native_sim      # Explicit
```

The simulator runs directly on your Linux host.

---

## Adding New Boards

To add support for a new board:

1. **Create board configuration:**
   ```
   boards/<board_id>.conf
   boards/<board_id>.overlay
   ```

2. **Update build.sh:**
   - Add to `BOARD_MAP`
   - Add to `BOARD_CHIP`
   - Add to `BOARD_DESC`

3. **Add flash function if needed** (for new chip families)

---

## Feature Compatibility Matrix

| Feature | ESP32-S3 | ESP32 | nRF54L15 | STM32U5 |
|---------|----------|-------|----------|---------|
| WiFi | ✅ | ✅ | ❌ | ✅* |
| BLE | ✅ | ✅ | ✅ | ✅ |
| 802.15.4 | ❌ | ❌ | ✅ | ❌ |
| Thread/Matter | ❌ | ❌ | ✅ | ❌ |
| USB | ✅ | ❌ | ✅ | ✅ |
| Display | ✅ | ✅ | ❌ | ❌ |
| SD Card | ✅ | ✅ | ❌ | ✅ |
| CAN | ❌ | ❌ | ❌ | ✅ |
| Sensors | ❌ | ❌ | ✅** | ✅ |

\* Via external MXCHIP module  
\** LSM6DSO IMU on board
