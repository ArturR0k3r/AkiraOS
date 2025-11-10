# AkiraOS Quick Start Guide

**Get up and running in 5 minutes!** 🚀

---

## Prerequisites

```bash
# Install required tools
pip install esptool west

# Verify installation
esptool version
west --version
```

---

## 🎯 Option 1: ESP32-S3 Console (Recommended)

**For full Akira Console experience with display and UI**

```bash
# 1. Clone and setup
cd ~
mkdir Akira && cd Akira
git clone <your-repo> AkiraOS
cd AkiraOS
west init -l .
cd .. && west update

# 2. Build MCUboot + AkiraOS
cd AkiraOS
./build_both.sh esp32s3

# 3. Connect ESP32-S3 DevKitM via USB

# 4. Flash
./flash.sh

# 5. Monitor
west espmonitor
```

**That's it!** 🎉

---

## 🎯 Option 2: ESP32-C3 Modules

**For sensor modules and wireless peripherals only**

```bash
# 1. Setup (same as above)
cd ~/Akira/AkiraOS

# 2. Build for ESP32-C3
./build_both.sh esp32c3

# 3. Connect ESP32-C3 DevKitM via USB

# 4. Flash
./flash.sh --platform esp32c3

# 5. Monitor
west espmonitor
```

**⚠️ Note:** ESP32-C3 is for **Akira Modules Only**, not Console!

---

## 🎯 Option 3: Native Simulation

**For development and testing without hardware**

```bash
# 1. Setup (same as above)
cd ~/Akira/AkiraOS

# 2. Build and run
./build_and_run.sh
```

---

## 🔧 Common Commands

### Building

```bash
# Build all platforms
./build_all.sh

# Build specific platform
./build_all.sh esp32s3    # ESP32-S3 Console
./build_all.sh esp32c3    # ESP32-C3 Modules
./build_all.sh native_sim # Native simulation

# Build with MCUboot
./build_both.sh esp32s3        # Build bootloader + app
./build_both.sh esp32s3 clean  # Clean and build
```

### Flashing

```bash
# Auto-detect and flash
./flash.sh

# Flash specific platform
./flash.sh --platform esp32s3
./flash.sh --platform esp32c3

# Flash only app (faster updates)
./flash.sh --app-only

# Flash to specific port
./flash.sh --port /dev/ttyUSB0
```

### Monitoring

```bash
# Using west (recommended)
west espmonitor
west espmonitor --port /dev/ttyUSB0

# Using screen
screen /dev/ttyUSB0 115200
# Exit: Ctrl+A then K then Y

# Using picocom  
picocom -b 115200 /dev/ttyUSB0
# Exit: Ctrl+A then Ctrl+X
```

---

## 🧪 Testing Sensor Modules

After flashing, test sensors in the shell:

```bash
# NRF24L01 (2.4GHz Wireless)
akira> nrf24 init
akira> nrf24 status
akira> nrf24 rx 76    # Set channel
akira> nrf24 listen   # Start listening

# LSM6DS3 (6-axis IMU)
akira> lsm6ds3 init
akira> lsm6ds3 read
akira> lsm6ds3 config accel 4  # ±4g range
akira> lsm6ds3 config gyro 500 # 500 dps

# INA219 (Power Monitor)
akira> ina219 init
akira> ina219 read
akira> ina219 calibrate 3.2 0.1  # 3.2V, 0.1Ω shunt
```

---

## 📊 Platform Comparison

| Feature | ESP32-S3 | ESP32 | ESP32-C3 | Native |
|---------|----------|-------|----------|--------|
| **Console** | ✅ Primary | ✅ Legacy | ❌ No | ❌ No |
| **Display** | ✅ ILI9341 | ✅ ILI9341 | ❌ No | ❌ No |
| **Sensors** | ✅ All | ✅ All | ✅ All | ❌ No |
| **Wi-Fi/BLE** | ✅ Yes | ✅ Yes | ✅ Yes | ❌ No |
| **WASM** | ✅ Yes | ✅ Limited | ⚠️ Limited | ✅ Yes |
| **Cores** | 2 | 2 | 1 | Host |
| **SRAM** | 512KB | 520KB | 400KB | Host |
| **PSRAM** | 8MB | 0 | 0 | Host |
| **Use Case** | Primary Console | Legacy Console | Modules Only | Development |

---

## 🆘 Quick Troubleshooting

### Build Fails

```bash
# Clean everything
cd ~/Akira
rm -rf build build-* AkiraOS/build
cd AkiraOS
./build_both.sh esp32s3 clean
```

### Flash Permission Denied

```bash
# Add user to dialout group
sudo usermod -a -G dialout $USER
# Log out and back in
```

### Can't Find Port

```bash
# List available ports
ls /dev/ttyUSB* /dev/ttyACM*

# Specify port manually
./flash.sh --port /dev/ttyUSB0
```

### ESP32-C3 Crashes

**Are you trying to use Console features on ESP32-C3?**

ESP32-C3 is for **Akira Modules Only**!
- ❌ Cannot run Akira Console
- ❌ No display support  
- ✅ Perfect for sensor modules

Use ESP32-S3 for Console instead.

---

## 📚 Learn More

- **[BUILD_SCRIPTS.md](BUILD_SCRIPTS.md)** - Complete build script guide
- **[BUILD_PLATFORMS.md](BUILD_PLATFORMS.md)** - Detailed platform comparison
- **[SENSOR_INTEGRATION.md](SENSOR_INTEGRATION.md)** - Sensor module guide
- **[README.md](../README.md)** - Full project documentation

---

## 🎮 What's Next?

1. **Test the Console UI**
   - Navigate with buttons
   - Try different apps
   - Check system info

2. **Test Sensor Modules**
   - Initialize sensors
   - Read values
   - Configure settings

3. **Try OTA Updates**
   - Connect to Wi-Fi
   - Upload new firmware
   - Test rollback

4. **Develop Your Own Module**
   - Create new sensor driver
   - Add shell commands
   - Integrate with UI

---

## 💡 Pro Tips

1. **Quick App Updates:** Use `./flash.sh --app-only` to skip flashing MCUboot (saves time!)

2. **Multi-Device Development:** Use `--port` to flash multiple devices:
   ```bash
   ./flash.sh --port /dev/ttyUSB0 --platform esp32s3  # Console
   ./flash.sh --port /dev/ttyUSB1 --platform esp32c3  # Module
   ```

3. **Native Testing First:** Always test on native_sim before flashing hardware:
   ```bash
   ./build_and_run.sh  # Test logic first
   ./build_both.sh esp32s3  # Then build for hardware
   ```

4. **Use Help Commands:** All scripts have help:
   ```bash
   ./build_all.sh help
   ./build_both.sh help
   ./flash.sh --help
   ```

5. **Monitor During Flash:** Open monitor in another terminal to see boot immediately:
   ```bash
   # Terminal 1
   west espmonitor
   
   # Terminal 2
   ./flash.sh --app-only
   ```

---

## 🤝 Need Help?

- 📖 Read [BUILD_SCRIPTS.md](BUILD_SCRIPTS.md) for detailed examples
- 🐛 Check [troubleshooting.md](troubleshooting.md) for common issues
- 🔍 See [BUILD_PLATFORMS.md](BUILD_PLATFORMS.md) for platform details
- 💬 Join community discussions
- 🚨 Report issues on GitHub

---

**Happy Hacking!** 🚀✨
