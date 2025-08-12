## **Architecture**

```c
┌─────────────────┐
│   WASM Apps     │ ← Игры и инструменты
├─────────────────┤
│   OCRE Runtime  │ ← Изоляция и безопасность  
├─────────────────┤
│ WASM-Micro-RT   │ ← WASM исполнение
├─────────────────┤
│   Zephyr OS     │ ← RTOS и драйверы
├─────────────────┤
│   ESP32 HAL     │ ← Железо
└─────────────────┘
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

## GPIO Pinout Configuration

### ESP32 GPIO Mapping

| Component | Function | GPIO Pin | Notes |
| --- | --- | --- | --- |
| **Display (ILI9341)** |  |  |  |
|  | MOSI (SDI) | GPIO23 | SPI Data Out |
|  | MISO (SDO) | GPIO19 | SPI Data In |
|  | SCK | GPIO18 | SPI Clock |
|  | CS | GPIO5 | Chip Select |
|  | DC | GPIO2 | Data/Command |
|  | RESET | GPIO4 | Display Reset |
|  | LED (Backlight) | GPIO16 | PWM Control |
| **Touch Controller (XPT2046)** |  |  |  |
|  | T_CLK | GPIO18 | Shared with display SCK |
|  | T_CS | GPIO17 | Touch Chip Select |
|  | T_DIN | GPIO23 | Shared with display MOSI |
|  | T_DO | GPIO19 | Shared with display MISO |
|  | T_IRQ | GPIO21 | Touch Interrupt |
| **SD Card** |  |  |  |
|  | SD_CS | GPIO15 | SD Card Chip Select |
|  | SD_MOSI | GPIO23 | Shared with SPI |
|  | SD_MISO | GPIO19 | Shared with SPI |
|  | SD_SCK | GPIO18 | Shared with SPI |
| **Control Buttons** |  |  |  |
|  | KEY1 (UP) | GPIO36 | Input only, pull-up |
|  | KEY2 (RIGHT) | GPIO39 | Input only, pull-up |
|  | KEY3 (DOWN) | GPIO34 | Input only, pull-up |
|  | KEY4 (LEFT) | GPIO35 | Input only, pull-up |
|  | KEY5 | GPIO32 | General purpose |
|  | KEY6 | GPIO33 | General purpose |
|  | KEY7 | GPIO25 | General purpose |
|  | KEY8 | GPIO26 | General purpose |
|  | KEY_SETTINGS | GPIO27 | Settings button |
|  | KEY_ON/OFF | GPIO14 | Power button |
| **Audio** |  |  |  |
|  | SOUND | GPIO22 | PWM/I2S Audio Out |
| **System** |  |  |  |
|  | EN (Reset) | EN | Hardware reset |
|  | GPIO0 | GPIO0 | Boot mode select |
| **UART (Programming)** |  |  |  |
|  | RX | GPIO3 | UART0 RX |
|  | TX | GPIO1 | UART0 TX |
| **Status LEDs** |  |  |  |
|  | STATUS (Blue) | GPIO12 | System status |
|  | CHARGE (Yellow) | GPIO13 | Battery charging |
| **Expansion Header** |  |  |  |
|  | GPIO4 | GPIO4 | Available for expansion |
|  | GPIO6 | GPIO6 | Available for expansion |
|  | GPIO7 | GPIO7 | Available for expansion |
|  | GPIO8 | GPIO8 | Available for expansion |
|  | GPIO10 | GPIO10 | Available for expansion |
|  | GPIO11 | GPIO11 | Available for expansion |

### Power Management

| Component | Voltage | Current | Notes |
| --- | --- | --- | --- |
| ESP32 Core | 3.3V | ~240mA active | Deep sleep: ~10µA |
| Display | 3.3V | ~20mA | Backlight: ~100mA |
| Battery | 3.7V Li-Po | 1000-2000mAh | Via MCP73832T charger |
| USB Power | 5V | 500mA max | Type-C connector |
