# Akira Module System

## Overview

The Akira Module System enables seamless integration of Akira hardware and controls into external projects. This modular architecture allows you to embed Akira functionality into your custom creations with minimal effort.

**Note:** This is core AkiraOS functionality located in `src/akira_modules/`, not a third-party module. The `modules/` directory is reserved for third-party dependencies like WASM-Micro-Runtime and OCRE.

**Designed for Makers, by Makers.**

## Features

- 🔌 **Plug-and-Play Integration** - Easy module registration system
- 🔄 **Multiple Communication Interfaces** - UART, SPI, I2C, Network
- 📡 **Event-Driven Architecture** - Broadcast events across modules
- 🎮 **Hardware Control** - Display, buttons, GPIO, audio, sensors
- 🌐 **Network Ready** - WiFi/Ethernet communication built-in
- 📦 **Lightweight** - Minimal memory footprint
- 🛠️ **Customizable** - Create your own modules easily

## Quick Start

### 1. Enable Akira Module System

In your `prj.conf`:

```conf
CONFIG_AKIRA_MODULE=y
CONFIG_AKIRA_MODULE_COMM_UART=y
CONFIG_AKIRA_MODULE_COMM_NETWORK=y
CONFIG_AKIRA_MODULE_HW_DISPLAY=y
CONFIG_AKIRA_MODULE_HW_BUTTONS=y
```

### 2. Create a Module

```c
#include <akira_module.h>

static int my_module_init(void *user_data)
{
    printk("My module initialized!\n");
    return 0;
}

static int my_module_command(const char *command, void *data, 
                              size_t len, void *user_data)
{
    if (strcmp(command, "hello") == 0) {
        printk("Hello from my module!\n");
        return 0;
    }
    return -ENOTSUP;
}

static int my_module_event(const char *event, void *data,
                            size_t len, void *user_data)
{
    printk("Received event: %s\n", event);
    return 0;
}

AKIRA_MODULE_DEFINE(my_module,
                    AKIRA_MODULE_TYPE_CUSTOM,
                    my_module_init,
                    NULL,
                    my_module_command,
                    my_module_event,
                    NULL);
```

### 3. Use the Module

```c
#include <akira_module.h>

void app_main(void)
{
    /* Initialize module system */
    akira_module_init();
    
    /* Start communication */
    akira_module_start_comm();
    
    /* Send command to module */
    akira_module_send_command("my_module", "hello", NULL, 0);
    
    /* Broadcast event */
    akira_module_broadcast_event("system_ready", NULL, 0);
}
```

## Module Types

| Type | Description | Use Case |
|------|-------------|----------|
| `AKIRA_MODULE_TYPE_DISPLAY` | Display control | Show UI, graphics, text |
| `AKIRA_MODULE_TYPE_INPUT` | Button/input handling | Read button states, handle input |
| `AKIRA_MODULE_TYPE_AUDIO` | Audio control | Play sounds, record audio |
| `AKIRA_MODULE_TYPE_STORAGE` | Storage access | Read/write files to SD card |
| `AKIRA_MODULE_TYPE_NETWORK` | Network communication | WiFi, HTTP, WebSocket |
| `AKIRA_MODULE_TYPE_GPIO` | GPIO control | Control external pins |
| `AKIRA_MODULE_TYPE_SENSOR` | Sensor data | Read temperature, accelerometer, etc. |
| `AKIRA_MODULE_TYPE_CUSTOM` | Custom functionality | Your own modules |

## Communication Interfaces

### UART Communication

```c
const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart0));
akira_module_set_comm_interface(AKIRA_COMM_UART, uart);
```

### Network Communication

```c
akira_module_set_comm_interface(AKIRA_COMM_NETWORK, NULL);
/* Network device auto-detected */
```

## Example Modules

### Display Module

```c
#include <akira_module.h>
#include <zephyr/display/display.h>

static int display_module_command(const char *command, void *data,
                                   size_t len, void *user_data)
{
    if (strcmp(command, "clear") == 0) {
        /* Clear display */
        return 0;
    } else if (strcmp(command, "text") == 0) {
        /* Draw text: data contains text string */
        return 0;
    }
    return -ENOTSUP;
}

AKIRA_MODULE_DEFINE(display,
                    AKIRA_MODULE_TYPE_DISPLAY,
                    NULL,
                    NULL,
                    display_module_command,
                    NULL,
                    NULL);
```

### Button Module

```c
#include <akira_module.h>
#include <zephyr/input/input.h>

static int button_module_command(const char *command, void *data,
                                  size_t len, void *user_data)
{
    if (strcmp(command, "read") == 0) {
        /* Read button states and return in data */
        uint32_t *buttons = (uint32_t *)data;
        *buttons = read_button_states();
        return 0;
    }
    return -ENOTSUP;
}

AKIRA_MODULE_DEFINE(buttons,
                    AKIRA_MODULE_TYPE_INPUT,
                    NULL,
                    NULL,
                    button_module_command,
                    NULL,
                    NULL);
```

### GPIO Module

```c
#include <akira_module.h>
#include <zephyr/drivers/gpio.h>

struct gpio_command {
    uint32_t pin;
    uint32_t value;
};

static int gpio_module_command(const char *command, void *data,
                                size_t len, void *user_data)
{
    struct gpio_command *cmd = (struct gpio_command *)data;
    
    if (strcmp(command, "set") == 0) {
        /* Set GPIO pin */
        return gpio_pin_set(gpio_dev, cmd->pin, cmd->value);
    } else if (strcmp(command, "get") == 0) {
        /* Get GPIO pin state */
        cmd->value = gpio_pin_get(gpio_dev, cmd->pin);
        return 0;
    }
    return -ENOTSUP;
}

AKIRA_MODULE_DEFINE(gpio,
                    AKIRA_MODULE_TYPE_GPIO,
                    NULL,
                    NULL,
                    gpio_module_command,
                    NULL,
                    NULL);
```

## Integration Examples

### Arduino Project Integration

Connect Akira to Arduino via UART and control it:

```cpp
// Arduino code
void setup() {
    Serial.begin(115200);
}

void loop() {
    // Send command to Akira
    Serial.println("{\"module\":\"display\",\"command\":\"text\",\"data\":\"Hello!\"}");
    delay(1000);
}
```

### Raspberry Pi Integration

Control Akira from Python:

```python
import serial
import json

akira = serial.Serial('/dev/ttyUSB0', 115200)

# Send command
cmd = {
    "module": "buttons",
    "command": "read"
}
akira.write(json.dumps(cmd).encode() + b'\n')

# Read response
response = json.loads(akira.readline())
print(f"Button state: {response['data']}")
```

### ESP32/ESP8266 Integration

Network-based control:

```cpp
#include <WiFi.h>
#include <HTTPClient.h>

void sendCommandToAkira(String command) {
    HTTPClient http;
    http.begin("http://akira.local/api/command");
    http.addHeader("Content-Type", "application/json");
    
    String payload = "{\"module\":\"display\",\"command\":\"" + command + "\"}";
    int httpCode = http.POST(payload);
    
    http.end();
}
```

## Protocol Format

### JSON Protocol (Default)

Commands:
```json
{
    "module": "display",
    "command": "text",
    "data": "Hello World"
}
```

Responses:
```json
{
    "status": "ok",
    "module": "display",
    "data": null
}
```

Events:
```json
{
    "type": "event",
    "event": "button_pressed",
    "data": {
        "button": "A",
        "state": "pressed"
    }
}
```

## Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_AKIRA_MODULE_MAX_MODULES` | 16 | Maximum registered modules |
| `CONFIG_AKIRA_MODULE_MAX_NAME_LEN` | 32 | Maximum module name length |
| `CONFIG_AKIRA_MODULE_THREAD_STACK_SIZE` | 2048 | Manager thread stack size |
| `CONFIG_AKIRA_MODULE_THREAD_PRIORITY` | 5 | Manager thread priority |

## Use Cases

### 🎮 Game Controller Extension
- Use Akira as a wireless game controller
- Connect to PC/console via WiFi
- Custom button mapping

### 🏠 Home Automation Hub
- Control Akira from home automation system
- Display sensor data on Akira screen
- Use Akira buttons to trigger automations

### 🤖 Robot Control Interface
- Mount Akira on robot
- Control robot movements via Akira buttons
- Display robot status on Akira screen

### 📊 Data Logger Display
- Receive sensor data from external devices
- Display real-time graphs on Akira
- Store data to SD card

### 🎵 MIDI Controller
- Use Akira buttons as MIDI input
- Display music info on screen
- Network MIDI over WiFi

## API Reference

See `include/akira_module.h` for complete API documentation.

## Contributing

We welcome contributions! Create your own modules and share them with the community.

## License

Apache-2.0

---

**Akira Console: Your Project, Your Rules.**
