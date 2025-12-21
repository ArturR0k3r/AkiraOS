# Akira-Micro Hardware Integration Test

This test validates the core hardware features of the Akira-Micro device.

## Hardware Tested

### ✅ GPIO & Buttons
- **KEY_1** (IO35) - DPAD Button 1
- **KEY_2** (IO34) - DPAD Button 2  
- **KEY_3** (IO39) - DPAD Button 3
- **KEY_4** (IO36) - DPAD Button 4
- **KEY_5** (IO14) - DPAD Button 5
- **KEY_6** (IO13) - DPAD Button 6

### ✅ Status LED
- **Blue LED** (IO32) - Visual feedback indicator
  - Slow blink (1Hz) when idle
  - Fast blink when buttons pressed
  - Blink speed increases with more buttons pressed

### ✅ SD Card (SPI)
- **CS** (IO0) - Chip Select
- **MISO** (IO5) - SPI Data Out
- **MOSI** (IO19) - SPI Data In
- **SCK** (IO18) - SPI Clock

### 🔧 Other Hardware (not tested yet)
- I2C OLED Display (IO21 SDA, IO22 SCL)
- nRF24L01 modules (CE_1 IO17, CE_2 IO16)
- CC1101 RF module (CS IO23)

## Hardware Connections

### Akira-Micro ESP32 Module (U2)

Based on `docs/HARDWARE.md`:

```
ESP32 Pin Map:
├── I2C (OLED)
│   ├── IO21 → SSD1306 SDA
│   └── IO22 → SSD1306 SCL
├── SPI (Shared)
│   ├── IO5  → MISO
│   ├── IO18 → SCK
│   └── IO19 → MOSI
├── Chip Selects
│   ├── IO0  → SD Card CS
│   ├── IO23 → CC1101 CS
│   ├── IO17 → nRF24L01 U9 CE
│   └── IO16 → nRF24L01 U1 CE
├── Buttons (Pull-Up, Active Low)
│   ├── IO35 → KEY_1
│   ├── IO34 → KEY_2
│   ├── IO39 → KEY_3
│   ├── IO36 → KEY_4
│   ├── IO14 → KEY_5
│   └── IO13 → KEY_6
└── LED
    └── IO32 → Blue Status LED (470Ω resistor)
```

## Building & Flashing

### Prerequisites
```bash
# Zephyr SDK and West tool installed
# ESP32 toolchain configured
```

### Build
```bash
cd tests/integration
./build_akira_micro_test.sh
```

### Flash to Device
```bash
west flash -d ../build_akira_micro_test
```

### Monitor Serial Output
```bash
west espressif monitor -d ../build_akira_micro_test
```

### All-in-One (Flash + Monitor)
```bash
west flash -d ../build_akira_micro_test && west espressif monitor -d ../build_akira_micro_test
```

## Expected Test Output

```
===========================================
  Akira-Micro Hardware Integration Test
===========================================
[00:00:00.000,000] <inf> akira_micro_test: GPIO initialized successfully

=== SD Card Test ===
[00:00:01.000,000] <inf> akira_micro_test: SD card mounted successfully
[00:00:01.050,000] <inf> akira_micro_test: Wrote 26 bytes to /SD:/akira_test.txt
[00:00:01.100,000] <inf> akira_micro_test: Read 26 bytes: Akira-Micro SD Card Test
[00:00:01.150,000] <inf> akira_micro_test: SD card test PASSED!
[00:00:01.200,000] <inf> akira_micro_test: SD card unmounted
[00:00:01.250,000] <inf> akira_micro_test: ✓ SD Card: PASS

=== Button Test (press buttons to see status) ===
[00:00:01.300,000] <inf> akira_micro_test: Press any combination of KEY1-KEY6 buttons
[00:00:01.350,000] <inf> akira_micro_test: LED will blink faster when buttons are pressed

[00:00:01.450,000] <inf> akira_micro_test: Buttons: KEY1=0 KEY2=0 KEY3=0 KEY4=0 KEY5=0 KEY6=0
[00:00:01.550,000] <inf> akira_micro_test: Buttons: KEY1=1 KEY2=0 KEY3=0 KEY4=0 KEY5=0 KEY6=0
[00:00:01.650,000] <inf> akira_micro_test: Buttons: KEY1=1 KEY2=1 KEY3=0 KEY4=0 KEY5=0 KEY6=0
...
```

## Test Success Criteria

### ✅ PASS Conditions
1. **SD Card Test**
   - Successfully mounts SD card
   - Creates test file
   - Writes data to file
   - Reads data back correctly
   - Data verification passes

2. **Button Test**
   - All 6 buttons read correctly
   - Button states update in real-time
   - Active-low logic works (0 = pressed, 1 = released)

3. **LED Test**
   - LED blinks slowly when idle
   - LED blinks faster when buttons pressed
   - Blink rate correlates with button count

### ❌ FAIL Conditions
- SD card fails to mount
- File operations fail
- Button states don't change when pressed
- LED doesn't blink
- Serial output shows errors

## Troubleshooting

### SD Card Issues
- Ensure SD card is formatted as FAT32
- Check SPI connections (IO5, IO18, IO19, IO0)
- Verify 3.3V power supply is stable
- Try different SD card (some cards have compatibility issues)

### Button Issues
- Verify pull-up resistors are present
- Check GPIO pins are not used elsewhere
- Test continuity of button connections
- Ensure buttons are active-low

### LED Issues
- Check IO32 connection to LED
- Verify 470Ω current-limiting resistor
- Ensure LED polarity is correct

### Serial Port Issues
```bash
# Find ESP32 serial port
ls /dev/ttyUSB* /dev/ttyACM*

# Give permissions
sudo usermod -a -G dialout $USER
# Log out and back in for changes to take effect
```

## Next Steps

After successful basic hardware test:

1. **Add I2C OLED test** - Display graphics on SSD1306
2. **Add nRF24L01 test** - Wireless communication
3. **Add CC1101 test** - Sub-GHz RF transceiver
4. **Integration test** - All peripherals working together

## Notes

- Test uses FatFS for SD card access
- Buttons use internal pull-ups (active low)
- LED uses GPIO bit-banging (no PWM yet)
- SPI speed set to 24MHz for SD card compatibility
- UART baud rate: 115200

## References

- Hardware documentation: `docs/HARDWARE.md`
- ESP32 DevKit pinout: ESP32-WROOM-32 datasheet
- Zephyr GPIO driver: `zephyr/drivers/gpio/`
- Zephyr SD/MMC: `zephyr/drivers/disk/`
