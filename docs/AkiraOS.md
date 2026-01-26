## **Architecture**

```c
┌─────────────────────────────┐
│      AkiraOS WASM Apps      │ ← Games, Tools, Utilities, User Apps
├─────────────────────────────┤
│      Akira Runtime (WAMR)   │ ← WebAssembly execution, App sandbox
├─────────────────────────────┤
│    WASM-Micro-RT            │ ← WASM Execution Engine for embedded
├─────────────────────────────┤
│      Akira Shell            │ ← Command-line, Debug Console, Scripting
├─────────────────────────────┤
│      Cloud Client           │ ← Unified messaging: Cloud, BT App, Web, USB
├─────────────────────────────┤
│      OTA Manager            │ ← Multi-transport: HTTP, BLE, USB, Cloud
├─────────────────────────────┤
│      Settings Module        │ ← Persistent User/Device Settings
├─────────────────────────────┤
│    System Manager (NEW)     │ ← Config-driven Init, Event Bus, Lifecycle
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

### Core Management System (NEW)

AkiraOS now features a modular initialization system with three key managers:

**System Manager** - Orchestrates the entire system
- Configuration-driven initialization via Kconfig
- Declarative init table with dependency priorities
- Graceful degradation for optional features
- System lifecycle coordination (boot → run → shutdown)

**Hardware Manager** - Hardware orchestration
- Leverages existing HAL and driver_registry
- Platform-specific initialization (ESP32, nRF, STM32)
- Display, sensor, and RF module setup
- Runtime driver loading and registration

**Network Manager** - Connectivity coordination
- WiFi, Bluetooth, USB management
- Integration with settings for credentials
- Event-driven network state changes
- HTTP server lifecycle management

**Event Bus** - Inter-module communication
- Pub/sub pattern for loose coupling
- System events: NETWORK_UP, OTA_PROGRESS, STORAGE_READY, etc.
- Thread-safe message queue
- Enables reactive, event-driven architecture

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

