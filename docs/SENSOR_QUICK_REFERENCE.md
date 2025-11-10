# Sensor Quick Reference Card

> **Platform Support:**  
> - ✅ ESP32-S3 (Akira Console - Primary)  
> - ✅ ESP32 (Akira Console - Legacy)  
> - ✅ ESP32-C3 (Akira Modules Only - Remote sensors)  
> 
> See [BUILD_PLATFORMS.md](BUILD_PLATFORMS.md) for platform selection guide.

## NRF24L01+ Wireless Transceiver

**Interface:** SPI + GPIO (CE, CSN)  
**I/O Voltage:** 3.3V  
**Frequency:** 2.4-2.525 GHz (125 channels)  
**Data Rate:** 250kbps, 1Mbps, 2Mbps  
**Range:** 100m (line of sight, high power)  
**Power:** 12mA TX, 13.5mA RX, 900nA sleep  

### Quick Commands
```bash
nrf_init                    # Initialize
nrf_channel 76              # Set channel (0-125)
nrf_power 3                 # Max power (0-3)
nrf_tx 0xE7E7E7E7E7         # TX mode
nrf_rx 1 0xE7E7E7E7E7       # RX mode pipe 1
nrf_send "Hello"            # Transmit
nrf_recv                    # Receive
```

### Pin Connections (ESP32-S3 - Akira Console)
```
NRF24L01+    ESP32-S3
---------    ---------
VCC     -->  3.3V
GND     -->  GND
CE      -->  GPIO 4
CSN     -->  GPIO 5
SCK     -->  GPIO 12 (SPI2_SCK)
MOSI    -->  GPIO 11 (SPI2_MOSI)
MISO    -->  GPIO 13 (SPI2_MISO)
IRQ     -->  (optional)
```

---

## LSM6DS3 6-Axis IMU (Replaces MPU6050)

**Interface:** I2C  
**I2C Address:** 0x6A (primary), 0x6B (secondary)  
**Voltage:** 1.71-3.6V  
**Accel Range:** ±2/±4/±8/±16 g  
**Gyro Range:** ±250/±500/±1000/±2000 dps  
**Power:** 0.9mA active, 3µA sleep  
**Update Rate:** 12.5Hz - 6.66kHz  

### Quick Commands
```bash
imu_init                    # Initialize
imu_read                    # Read all (accel+gyro+temp)
imu_accel                   # Read accelerometer
imu_gyro                    # Read gyroscope
imu_temp                    # Read temperature
imu_continuous on           # Start 10Hz updates
imu_continuous off          # Stop updates
```

### Output Format
```
Accel: X=  12.34 Y= -56.78 Z= 901.23 mg
Gyro:  X=   5.67 Y=  -8.90 Z=   1.23 dps
Temperature: 25.5°C
```

### Pin Connections (ESP32-S3)
```
LSM6DS3      ESP32-S3
---------    ---------
VDD     -->  3.3V
GND     -->  GND
SDA     -->  GPIO 8 (I2C0_SDA)
SCL     -->  GPIO 9 (I2C0_SCL)
INT1    -->  (optional)
INT2    -->  (optional)
```

---

## INA219 Current/Power Monitor

**Interface:** I2C  
**I2C Address:** 0x40-0x4F (A0/A1 configurable)  
**Bus Voltage:** 0-32V (0-16V or 0-32V range)  
**Shunt Voltage:** ±320mV max (with gain=8)  
**Current:** Up to 3.2A (with 0.1Ω shunt)  
**Resolution:** 9-12 bit ADC  
**Power:** 1mA active, 1µA sleep  

### Quick Commands
```bash
power_init                  # Init (0.1Ω, 3.2A)
power_init 0.05 5.0         # Custom (0.05Ω, 5A)
power_read                  # Read all
power_voltage               # Bus+shunt voltage
power_current               # Current only
power_power                 # Power only
power_monitor on            # Start 2Hz updates
power_monitor off           # Stop updates
power_sleep                 # Sleep mode
power_wake                  # Wake up
```

### Output Format
```
Bus Voltage:     3.300 V
Shunt Voltage:  10.50 mV
Current:       105.00 mA
Power:         346.50 mW
```

### Pin Connections (ESP32-S3)
```
INA219       ESP32-S3
---------    ---------
VCC     -->  3.3V or 5V
GND     -->  GND
SDA     -->  GPIO 8 (I2C0_SDA, shared)
SCL     -->  GPIO 9 (I2C0_SCL, shared)
A0      -->  GND (address selection)
A1      -->  GND (address selection)
```

### Shunt Connection
```
      ┌─────[0.1Ω Shunt]─────┐
      │                       │
Load+ ●───> INA219 IN+    OUT+●───> Power Supply+
      │                       │
      └───> INA219 IN-    OUT-●───> (connect to load)
```

---

## Configuration Summary

### Enable All Sensors (prj.conf or boards/sensors.conf)
```conf
# Drivers
CONFIG_NRF24L01=y
CONFIG_LSM6DS3=y
CONFIG_INA219=y

# Akira Modules
CONFIG_AKIRA_MODULE=y
CONFIG_AKIRA_MODULE_NRF24L01=y
CONFIG_AKIRA_MODULE_LSM6DS3=y
CONFIG_AKIRA_MODULE_INA219=y

# Interfaces
CONFIG_SPI=y
CONFIG_I2C=y
CONFIG_GPIO=y
```

### Build Commands
```bash
# Standard build
west build -b esp32s3_devkitm -p

# With sensor overlay
west build -b esp32s3_devkitm -p -- -DOVERLAY_CONFIG=boards/sensors.conf

# Flash
west flash

# Monitor
west espressif monitor
```

---

## C API Integration

### Event Handler Example
```c
#include <akira_module_core.h>

static void sensor_event_handler(const char *event_type,
                                  const uint8_t *data,
                                  size_t len)
{
    if (strcmp(event_type, AKIRA_MODULE_EVENT_SENSOR_DATA) == 0) {
        printk("Sensor: %.*s\n", len, data);
    }
}

void setup(void) {
    akira_module_register_event_handler(sensor_event_handler);
    akira_module_load("lsm6ds3");
    akira_module_start("lsm6ds3");
    akira_module_execute_command("imu_init", NULL);
}
```

---

## Troubleshooting

### NRF24L01+ Issues
- **No communication:** Check CE/CSN pins, verify 3.3V power
- **Short range:** Increase PA level, add 10µF cap to VCC
- **Data corruption:** Lower SPI speed, check connections

### LSM6DS3 Issues
- **Sensor not found:** Verify I2C address (try 0x6A and 0x6B)
- **Noisy readings:** Lower ODR, enable filtering
- **Wrong values:** Check accelerometer/gyro range settings

### INA219 Issues
- **Zero current:** Check shunt resistor connections
- **Overflow:** Reduce max_expected_current parameter
- **Wrong voltage:** Verify bus voltage range setting (16V vs 32V)
- **I2C error:** Check A0/A1 pins for correct address

### General I2C Issues
- **Bus error:** Add 4.7kΩ pull-ups to SDA/SCL
- **Multiple sensors:** Ensure unique I2C addresses
- **Speed issues:** Try lower I2C speed (100kHz)

---

## Performance Tips

1. **Power Optimization:**
   - Use sleep modes when idle
   - Lower ODR for LSM6DS3 when high speed not needed
   - Use triggered mode for INA219 instead of continuous

2. **Update Rates:**
   - LSM6DS3: 10Hz continuous (adjust in module code)
   - INA219: 2Hz monitoring (adjust in module code)
   - NRF24L01: On-demand (command-driven)

3. **Thread Priorities:**
   - Default module priority: 5
   - Continuous reading threads: 10
   - Adjust via `CONFIG_AKIRA_MODULE_THREAD_PRIORITY`

4. **Memory Usage:**
   - Each continuous thread: 2KB stack
   - Module system overhead: ~8KB
   - Sensor data buffers: ~100 bytes each

---

## Common Use Cases

### 1. Battery Monitor
```bash
power_init 0.1 2.0          # 100mΩ shunt, 2A max
power_monitor on            # Start monitoring
# Watch voltage/current/power every 500ms
```

### 2. Motion Detection
```bash
imu_init                    # Initialize IMU
imu_continuous on           # Start reading
# Parse accelerometer data for motion
```

### 3. Wireless Sensor Node
```bash
imu_init                    # Initialize sensor
nrf_init                    # Initialize radio
nrf_tx 0xE7E7E7E7E7         # Set destination
# Read sensor, format, transmit
```

### 4. Remote Control Receiver
```bash
nrf_init                    # Initialize radio
nrf_rx 1 0xE7E7E7E7E7       # Listen on pipe 1
nrf_recv                    # Wait for commands
# Parse and execute received commands
```

---

## Additional Resources

- **Full Documentation:** `src/akira_modules/examples/SENSOR_MODULES.md`
- **Integration Guide:** `src/akira_modules/examples/INTEGRATION_GUIDE.md`
- **System Overview:** `docs/SENSOR_INTEGRATION.md`
- **Datasheets:**
  - NRF24L01+: https://www.nordicsemi.com/products/nrf24-series
  - LSM6DS3: https://www.st.com/en/mems-and-sensors/lsm6ds3.html
  - INA219: https://www.ti.com/product/INA219

---

## Quick Sensor Test

Connect to serial console and run:
```bash
# Test all sensors
akira> imu_init
akira> imu_read
akira> power_init
akira> power_read
akira> nrf_init
akira> nrf_channel 76

# Start continuous monitoring
akira> imu_continuous on
akira> power_monitor on

# Stop monitoring
akira> imu_continuous off
akira> power_monitor off
```

---

**License:** Apache-2.0  
**Version:** 1.0.0  
**Last Updated:** November 2025
