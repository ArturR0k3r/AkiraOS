# Akira Module System - Quick Start for Makers

## Important Note

The Akira Module System is **core AkiraOS functionality** located in `src/akira_modules/`, not a third-party module. This distinction is important:

- **`src/`** - Core AkiraOS components (drivers, shell, settings, **akira_modules**)
- **`modules/`** - Third-party dependencies (WASM-Micro-Runtime, OCRE runtime)

## What Can You Build?

The Akira Module System turns your Akira Console into a versatile building block for any project:

- 🎮 **Wireless Game Controller** - Control your PC games or retro console
- 🏠 **Smart Home Display** - Show home automation status and controls
- 🤖 **Robot Control Panel** - Mount on your robot for remote control
- 📊 **Data Logger** - Display and log sensor data from Arduino/ESP32
- 🎵 **MIDI Controller** - Use as a music production tool
- 🔬 **Lab Equipment Interface** - Control test equipment with custom UI
- 🚗 **Car Dashboard** - Custom vehicle information display
- 📡 **IoT Gateway** - Bridge between devices with display feedback

## 5-Minute Setup

### Step 1: Enable in Your Project

Add to `prj.conf`:
```conf
CONFIG_AKIRA_MODULE=y
CONFIG_AKIRA_MODULE_COMM_UART=y
CONFIG_AKIRA_MODULE_HW_DISPLAY=y
CONFIG_AKIRA_MODULE_HW_BUTTONS=y
```

### Step 1: Copy Template
```bash
cp src/akira_modules/examples/module_template.c src/my_module.c
```

### Step 3: Build and Flash

```bash
west build -b esp32s3_devkitm/esp32s3/procpu
west flash
```

## Simple Examples

### Example 1: Arduino Control via UART

**Arduino Side:**
```cpp
void setup() {
    Serial.begin(115200);
}

void loop() {
    // Clear Akira display
    Serial.println("{\"module\":\"display\",\"command\":\"clear\"}");
    delay(100);
    
    // Show sensor reading
    float temp = readTemperature();
    String cmd = "{\"module\":\"display\",\"command\":\"text\",\"data\":\"";
    cmd += "Temp: " + String(temp) + "C\"}";
    Serial.println(cmd);
    
    delay(1000);
}
```

**Result:** Your Arduino controls Akira's display!

### Example 2: Python Control via Network

```python
import requests
import json

AKIRA_IP = "192.168.1.100"  # Your Akira's IP

def send_command(module, command, data=None):
    url = f"http://{AKIRA_IP}/api/command"
    payload = {
        "module": module,
        "command": command,
        "data": data
    }
    response = requests.post(url, json=payload)
    return response.json()

# Show text on Akira
send_command("display", "text", "Hello from Python!")

# Read button state
buttons = send_command("buttons", "read")
print(f"Buttons: {buttons}")

# Control GPIO pin
send_command("gpio", "set", {"pin": 5, "value": 1})
```

**Result:** Control Akira from your computer!

### Example 3: ESP32 Wireless Sensor Display

**ESP32 Side:**
```cpp
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>

DHT dht(4, DHT22);

void sendToAkira(float temp, float humidity) {
    HTTPClient http;
    http.begin("http://akira.local/api/sensor");
    
    String json = "{\"temperature\":" + String(temp) + 
                  ",\"humidity\":" + String(humidity) + "}";
    
    http.addHeader("Content-Type", "application/json");
    http.POST(json);
    http.end();
}

void loop() {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    sendToAkira(t, h);
    delay(2000);
}
```

**Result:** Wireless sensor display on Akira!

## Hardware Connections

### UART Connection
```
Arduino/ESP32     Akira Console
-----------       -------------
    TX     ---->      RX (GPIO16)
    RX     <----      TX (GPIO17)
   GND     ----      GND
```

### SPI Connection (High-Speed)
```
Arduino/ESP32     Akira Console
-----------       -------------
   MOSI    ---->      MOSI (GPIO11)
   MISO    <----      MISO (GPIO13)
   SCK     ---->      SCK (GPIO12)
    CS     ---->      CS (GPIO10)
   GND     ----      GND
```

### I2C Connection
```
Arduino/ESP32     Akira Console
-----------       -------------
   SDA     <--->      SDA (GPIO8)
   SCL     <--->      SCL (GPIO9)
   GND     ----      GND
```

## Common Commands

| Module | Command | Data | Description |
|--------|---------|------|-------------|
| display | clear | - | Clear screen |
| display | text | "x,y,Hello" | Show text at position |
| display | fill | 0xFFFF | Fill with color |
| buttons | read | - | Get button states |
| gpio | set | {pin:5, value:1} | Set GPIO pin |
| gpio | get | {pin:5} | Read GPIO pin |
| sensor | read | - | Read sensor data |

## Protocol Details

### JSON Command Format
```json
{
    "module": "display",
    "command": "text",
    "data": "10,20,Hello World"
}
```

### JSON Response Format
```json
{
    "status": "ok",
    "module": "display",
    "result": null
}
```

### Event Format
```json
{
    "type": "event",
    "event": "button_pressed",
    "data": {
        "button": "A",
        "timestamp": 12345
    }
}
```

## Tips for Success

1. **Start Simple** - Begin with display commands, then add complexity
2. **Use JSON** - It's human-readable and easy to debug
3. **Check Status** - Always verify module responses
4. **Event Driven** - Use events for real-time updates
5. **Test Connectivity** - Verify UART/Network before complex commands

## Troubleshooting

**Module not found?**
- Check if module is enabled in prj.conf
- Verify module is registered with `AKIRA_MODULE_DEFINE`

**UART not working?**
- Check baud rate (115200)
- Verify TX/RX are not swapped
- Test with simple "clear" command

**Network not responding?**
- Verify Akira IP address
- Check WiFi connection
- Test with ping first

**Commands not executing?**
- Verify JSON format
- Check module name spelling
- Enable debug logging

## Your Project, Your Rules

The Akira Module System is designed for maximum flexibility:

- ✅ Use any communication protocol
- ✅ Create custom modules easily  
- ✅ Mix and match functionality
- ✅ Scale from hobby to production
- ✅ Open source and hackable

## Community Examples

Share your projects! Visit:
- GitHub: github.com/ArturR0k3r/AkiraOS
- Discord: [Join our community]
- Forum: [Akira Makers Forum]

## Next Steps

1. Try the examples above
2. Create your first custom module
3. Integrate Akira into your project
4. Share your creation with the community!

---

**Akira Console: Designed for Makers, by Makers.**

Your Project. Your Rules. Your Akira.
