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
│      OTA Manager            │ ← Firmware Updates, MCUboot Integration
├─────────────────────────────┤
│      Settings Module        │ ← Persistent User/Device Settings
├─────────────────────────────┤
│      Networking Stack       │ ← WiFi, TCP/IP, HTTP, Web Server
├─────────────────────────────┤
│         Akira               │
│      graphic engine         │ ← Hardware drivers and libs, Framebuffer, UI Rendering ... 
│      and Hardware drivers   │
├─────────────────────────────┤
│      Zephyr OS              │ ← RTOS, Device Drivers, Kernel Services
├─────────────────────────────┤
│      ESP32 HAL              │ ← Hardware Abstraction, GPIO, SPI, UART, I2C
└─────────────────────────────┘
```

## AkiraOS State Diagram

```mermaid
stateDiagram-v2
    [*] --> PowerOff: System Off
    
    PowerOff --> BootLoader: Power Button Press
    BootLoader --> KernelInit: Hardware Check OK
    BootLoader --> ErrorState: Hardware Check Fail
    
    KernelInit --> RuntimeInit: Kernel Ready
    KernelInit --> ErrorState: Kernel Panic
    
    RuntimeInit --> Launcher: WASM Runtime Ready
    RuntimeInit --> ErrorState: Runtime Fail
    
    Launcher --> AppSelection: User Input
    Launcher --> SystemMenu: Settings Button
    Launcher --> Sleep: Idle Timeout
    Launcher --> Terminal: Hacker Mode Key
    
    AppSelection --> GameMode: Game Selected
    AppSelection --> ToolMode: Tool Selected
    AppSelection --> Launcher: Back Button
    
    GameMode --> GameRunning: WASM App Loaded
    GameMode --> ErrorState: App Load Fail
    GameRunning --> GamePaused: Menu Button
    GameRunning --> Launcher: Exit Game
    GameRunning --> ErrorState: App Crash
    GamePaused --> GameRunning: Resume
    GamePaused --> Launcher: Exit to Launcher
    
    ToolMode --> ToolRunning: Tool App Loaded
    ToolMode --> ErrorState: App Load Fail
    ToolRunning --> Launcher: Exit Tool
    ToolRunning --> ErrorState: Tool Crash
    
    Terminal --> TerminalActive: Terminal Loaded
    Terminal --> ErrorState: Terminal Fail
    TerminalActive --> Launcher: Exit Terminal
    TerminalActive --> NetworkMode: Network Tools
    NetworkMode --> TerminalActive: Back to Terminal
    
    SystemMenu --> Settings: Settings Selected
    SystemMenu --> About: About Selected
    SystemMenu --> PowerMenu: Power Options
    SystemMenu --> Launcher: Back
    
    Settings --> SettingsActive: Settings Loaded
    SettingsActive --> SystemMenu: Back
    SettingsActive --> Reboot: Apply & Reboot
    
    PowerMenu --> Sleep: Sleep Selected
    PowerMenu --> Reboot: Reboot Selected
    PowerMenu --> PowerOff: Shutdown Selected
    PowerMenu --> Launcher: Cancel
    
    Sleep --> Launcher: Wake Up (Any Button)
    Sleep --> PowerOff: Long Sleep Timeout
    
    Reboot --> BootLoader: System Restart
    
    ErrorState --> ErrorDisplay: Show Error
    ErrorDisplay --> Recovery: Recovery Attempt
    ErrorDisplay --> PowerOff: Critical Error
    Recovery --> Launcher: Recovery Success
    Recovery --> PowerOff: Recovery Fail
    
    note right of BootLoader
        Hardware initialization:
        - Display init
        - Touch calibration
        - SD card mount
        - Battery check
    end note
    
    note right of RuntimeInit
        WASM Runtime setup:
        - WAMR initialization
        - OCRE security setup
        - Memory allocation
        - API registration
    end note
    
    note right of GameRunning
        Isolated WASM execution:
        - Sandboxed environment
        - Resource limits
        - API access control
    end note
    
    note right of Terminal
        Hacker mode features:
        - Network scanning
        - Protocol analysis
        - System information
        - File management
    end note

```

## Hardware Configuration

### ESP32 DevKit C to ILI9341 Display Pin Mapping

#### SPI Display Connections
| ESP32 GPIO | ILI9341 Pin | Function | Notes |
|------------|-------------|----------|--------|
| GPIO23     | MOSI (SDI)  | SPI Data Out | Data from ESP32 to display |
| GPIO25     | MISO (SDO)  | SPI Data In  | Optional - for reading display |
| **GPIO19** | SCK         | SPI Clock    | **Main clock line** |
| GPIO22     | CS          | Chip Select  | Manual control |
| GPIO21     | DC          | Data/Command | Low=Command, High=Data |
| GPIO18     | RESET       | Reset        | Hardware reset |
| GPIO27     | LED         | Backlight    | PWM capable for dimming |

#### Gaming Control Buttons
| ESP32 GPIO | Button | Type | Notes |
|------------|--------|------|--------|
| GPIO12     | Power/Menu | Input | Pull-up enabled |
| GPIO16     | Settings | Input | Pull-up enabled |
| GPIO34     | D-Pad Up | Input Only | ⚠️ No pull-up (external required) |
| GPIO33     | D-Pad Down | Input | Pull-up enabled |
| GPIO32     | D-Pad Left | Input | Pull-up enabled |
| GPIO39     | D-Pad Right | Input Only | ⚠️ No pull-up (external required) |
| GPIO17     | A Button | Input | Pull-up enabled |
| GPIO14     | B Button | Input | Pull-up enabled |
| GPIO35     | X Button | Input Only | ⚠️ No pull-up (external required) |
| GPIO13     | Y Button | Input | Pull-up enabled |



| ESP32S3 GPIO | Button | Type | Notes |
|------------|--------|------|--------|
| GPIO7      | Power/Menu | Input | Pull-up enabled |
| GPIO42     | Settings | Input | Pull-up enabled |
| GPIO3      | D-Pad Up | Input Only | ⚠️ No pull-up (external required) |
| GPIO41     | D-Pad Down | Input | Pull-up enabled |
| GPIO47     | D-Pad Left | Input | Pull-up enabled |
| GPIO40     | D-Pad Right | Input Only | ⚠️ No pull-up (external required) |
| GPIO46     | A Button | Input | Pull-up enabled |
| GPIO14     | B Button | Input | Pull-up enabled |
| GPIO15     | X Button | Input Only | ⚠️ No pull-up (external required) |
| GPIO39     | Y Button | Input | Pull-up enabled |

