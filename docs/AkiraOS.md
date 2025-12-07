## **Architecture**

```c
┌─────────────────────────────┐
│      AkiraOS WASM Apps      │ ← Games, Tools, Utilities, User Apps
├─────────────────────────────┤
│      OCRE Runtime           │ ← Open Container Runtime (OCI/WASM), Security, Sandboxing
├─────────────────────────────┤
│    WASM-Micro-RT            │ ← WASM Execution Environment for OCRE
├─────────────────────────────┤
│      Akira Shell            │ ← Command-line, Debug Console, Scripting
├─────────────────────────────┤
│      Cloud Client           │ ← Unified messaging: Cloud, BT App, Web, USB
├─────────────────────────────┤
│      OTA Manager            │ ← Multi-transport: HTTP, BLE, USB, Cloud
├─────────────────────────────┤
│      Settings Module        │ ← Persistent User/Device Settings
├─────────────────────────────┤
│     Connectivity Layer      │ ← HID (KB/Gamepad), HTTP/WebSocket, BT, USB
├─────────────────────────────┤
│      Networking Stack       │ ← WiFi, TCP/IP, HTTP, Web Server
├─────────────────────────────┤
│         Akira               │
│      graphic engine         │ ← Hardware drivers and libs, Framebuffer, UI Rendering ... 
│      and Hardware drivers   │
├─────────────────────────────┤
│      Platform HAL           │ ← Hardware GPIO, SPI, Display Simulation
├─────────────────────────────┤
│       System HAL            │ ← Version, Uptime, Reboot, Device Info
├─────────────────────────────┤
│      Zephyr OS              │ ← RTOS, Device Drivers, Kernel Services
├─────────────────────────────┤
│      ESP32 HAL              │ ← Hardware Abstraction, GPIO, SPI, UART, I2C
└─────────────────────────────┘
```

## Connectivity Layer

AkiraOS provides a unified connectivity layer for:

### Cloud Client (Unified Messaging)
- **Multi-Source Routing**: Cloud, Bluetooth app (AkiraApp), Web server, USB
- **Message Categories**: System, OTA, App, Data, Control, Notify
- **App Handler**: Download and install WASM apps from any source
- **OTA Handler**: Firmware updates triggered from any source
- **Custom Handlers**: Register handlers for specific message types

### HID (Human Interface Device)
- **Keyboard Profile**: Send keystrokes over BLE/USB
- **Gamepad Profile**: Controller inputs (buttons, axes, D-pad)
- **Transport Abstraction**: BLE, USB, or simulated (native_sim)

### HTTP Server
- **Web Interface**: Configuration and status pages
- **REST API**: System control and monitoring
- **WebSocket**: Real-time bidirectional communication
- **OTA Upload**: Firmware updates via web interface

### Client Connectivity
- **WebSocket Client**: Connect to cloud servers (WSS)
- **CoAP Client**: IoT protocols with DTLS support
- **Auto-Reconnect**: Configurable retry with exponential backoff

### OTA Multi-Transport
- **HTTP**: Web server file upload
- **BLE**: Bluetooth OTA service
- **USB**: USB CDC ACM transfer
- **Cloud**: Via Cloud Client Future AkiraHub (stub)

