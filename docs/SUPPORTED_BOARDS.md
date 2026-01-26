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
| `b_u585i_iot02a` | b_u585i_iot02a | ST B-U585I-IOT02A | WiFi, BLE, Sensors |

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

## ST B-U585I-IOT02A (Discovery Kit for IoT) - EXPERIMENTAL

**Board ID:** `b_u585i_iot02a`  
**Status:** ✅ **BUILDS SUCCESSFULLY** - Minimal configuration working, WiFi driver pending

### Specifications
- **SoC:** STM32U585AIIx (ARM Cortex-M33 @ 160MHz)
- **Flash:** 2MB internal + 64MB Octo-SPI NOR external
- **RAM:** 768KB SRAM (192KB SRAM1 + 64KB SRAM2 + 512KB SRAM3)
- **Security:** TrustZone-M, STSAFE-A110 secure element
- **Connectivity:** MXCHIP EMW3080B (WiFi 802.11b/g/n + BLE 5.0), USB Type-C

### On-Board Sensors
| Sensor | Type | Interface |
|--------|------|-----------|
| HTS221 | Humidity & Temperature | I2C |
| LPS22HH | Pressure | I2C |
| ISM330DHCX | 6-axis IMU + ML Core | I2C |
| IIS2MDC | 3-axis Magnetometer | I2C |
| VL53L5CX | Time-of-Flight Multi-Zone | I2C |
| VEML3328 | Ambient Light & Color | I2C |
| MP23DB01HP | Digital MEMS Microphone | PDM |

### Current Build Status ✅
**Binary:** `/home/artur_ubuntu/Akira/build-b-u585i-iot02a/zephyr/zephyr.bin`
- **Flash Usage:** 205 KB / 416 KB (49.25%)
- **RAM Usage:** 180 KB / 768 KB (23.40%)
- **Status:** Successfully building minimal configuration

### Supported Features
- ✅ Platform-independent code (works with/without networking)
- ✅ Basic I2C/SPI/GPIO peripherals
- ✅ Onboard sensors (HTS221, LPS22HH, ISM330DHCX, IIS2MDC)
- ✅ USB Type-C console & shell
- ✅ Hardware Crypto (TinyCrypt AES, SHA-256)
- ✅ Settings persistence (NVS)
- ✅ LittleFS support
- ✅ TrustZone-M Security
- ✅ Secure Element (STSAFE-A110)
- ❌ WiFi 802.11 b/g/n - **MXCHIP driver not available in Zephyr**
- ❌ Bluetooth LE 5.0 - **MXCHIP driver required**
- ❌ Web Server - **Requires WiFi/networking**
- ❌ WASM Support - **WAMR needs sockets (requires networking)**
- ❌ OTA Updates - **Requires networking**

### Known Limitations & Blockers
1. **WiFi Driver:** MXCHIP EMW3080B has no upstream Zephyr driver - requires custom development (SPI-AT or native protocol)
2. **Flash Partition:** 416KB MCUboot slot insufficient for full AkiraOS with WASM (~800KB needed) - can't override base DTS without label conflicts
3. **WAMR Dependencies:** WebAssembly runtime requires socket support (CONFIG_NETWORKING=y), which requires WiFi driver
4. **No PSRAM:** Board has 768KB internal SRAM (sufficient for WASM heap), but flash code size is the bottleneck

### Build & Flash
```bash
# Build minimal configuration (works now - 205KB)
./build.sh -b b_u585i_iot02a

# Build with MCUboot bootloader
./build.sh -b b_u585i_iot02a -bl y

# Build and flash
./build.sh -b b_u585i_iot02a -r all

# Flash only via DFU (hold BOOT0 button while connecting USB)
./build.sh -b b_u585i_iot02a -r a

# Or with ST-LINK debugger
west flash -d build-b-u585i-iot02a
```

### Memory Configuration
- **Heap:** 128 KB (sufficient for sensors, crypto, shell)
- **Main Stack:** 8 KB
- **ISR Stack:** 4 KB
- **Flash Partitions:** 64KB boot + 416KB slot0 + 416KB slot1 + 64KB scratch + 64KB storage

### TODO / Future Work
- [ ] **MXCHIP WiFi Driver Development** (SPI-AT commands or native protocol) - enables full networking features
- [ ] **Investigate WAMR with minimal TCP/UDP stack** (no WiFi, sockets only) - may enable WASM without WiFi hardware
- [ ] **Upstream Zephyr patch for larger partitions** (896KB slots) - enables full AkiraOS with all features
- [ ] **External Octo-SPI flash support** (64MB MX25LM51245G) - additional storage for assets/WASM modules

### Programming Methods
1. **DFU Mode:** Hold BOOT0 button, connect USB-C, release button, run flash command
2. **ST-LINK:** Connect CN4 (STDC14), use `west flash` or STM32CubeProgrammer
2. **SWD:** Use ST-LINK/V3 via CN8 connector
3. **USB Virtual COM:** STLINK-V3E provides virtual COM port

### Memory Configuration
- **MCUboot:** 64KB (0x00000000 - 0x0000FFFF)
- **Slot 0 (Primary):** 416KB (0x00010000 - 0x00077FFF)
- **Slot 1 (Secondary):** 416KB (0x00078000 - 0x000DFFFF)
- **Scratch:** 64KB (0x000E0000 - 0x000EFFFF)
- **Storage (NVS):** 64KB (0x000F0000 - 0x000FFFFF)

**Note:** Partition layout from base Zephyr board definition may need adjustment for full AkiraOS features.

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

| Feature | ESP32-S3 | ESP32 | nRF54L15 | STWINBX1 | B-U585I |
|---------|----------|-------|----------|----------|---------|
| WiFi | ✅ | ✅ | ❌ | ✅* | ✅* |
| BLE | ✅ | ✅ | ✅ | ✅ | ✅* |
| 802.15.4 | ❌ | ❌ | ✅ | ❌ | ❌ |
| Thread/Matter | ❌ | ❌ | ✅ | ❌ | ❌ |
| USB | ✅ | ❌ | ✅ | ✅ | ✅ |
| Display | ✅ | ✅ | ❌ | ❌ | ❌ |
| SD Card | ✅ | ✅ | ❌ | ✅ | ✅ |
| CAN | ❌ | ❌ | ❌ | ✅ | ❌ |
| Sensors | ❌ | ❌ | ✅** | ✅ | ✅ |
| TrustZone | ❌ | ❌ | ✅ | ❌ | ✅ |
| Ext Flash | ❌ | ❌ | ❌ | ❌ | ✅*** |

\* Via external MXCHIP module  
\** LSM6DSO IMU on board  
\*** 64MB Octo-SPI NOR Flash
