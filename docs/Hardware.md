Akira-Console Pin Mapping

This device is built around the **ESP32-S3-DEVKITC-1-N8** module (**U8**), featuring communication modules (**LR1121**, **nRF24L01**), a **TFT LCD/Touch** screen, and a **MicroSD card**.

---

### 📍 ESP32-S3 (U8) Main Pin Assignments

| ESP32-S3 Pin (U8) | Pin Number | Peripheral/Component | Signal Name | Function |
| :---: | :---: | :---: | :---: | :---: |
| **LR1121 Module (U2)** | | | | |
| SPIIO7, GPIO36 | 33 | LR1121 (U2) | **MOSI** | SPI Data In |
| SPIDQS, GPIO37 | 34 | LR1121 (U2) | **MISO** | SPI Data Out |
| MTCK, GPIO39 | 36 | LR1121 (U2) | **SCK** | SPI Clock |
| MTDO, GPIO40 | 37 | LR1121 (U2) | **LR1121\_RST** | Reset Pin |
| MTDI, GPIO41 | 38 | LR1121 (U2) | **LR1121\_BUSY** | Busy Pin |
| MTMS, GPIO42 | 39 | LR1121 (U2) | **LR1121\_IRQ** | Interrupt Pin |
| **TFT LCD/Touch & SD Card** | | | | |
| RTC\_GPIO2, GPIO2 | 40 | TFT LCD/Touch | **SCK** | SPI Clock (Shared Bus) |
| RTC\_GPIO1, GPIO1 | 41 | TFT LCD/Touch | **MOSI** | MOSI (Shared Bus) |
| RTC\_GPIO21, GPIO21 | 27 | TFT LCD/Touch | **TFT\_CS** | Chip Select (ILI9341 LCD) |
| GPIO48 | 29 | TFT LCD/Touch | **TFT\_DC** | Data/Command (ILI9341 LCD) |
| GPIO38 | 35 | TFT LCD/Touch | **TFT\_LED** | Backlight/LED Control |
| GPIO46 | 14 | MicroSD Card (U7) | **SD\_CS** | Chip Select |
| **nRF24L01 (U1/U9) Selects** | | | | |
| GPIO47 | 28 | nRF24L01 (U1) | **nrf1\_CSN** | Chip Select |
| GPIO0, RTC\_GPIO0 | 31 | nRF24L01 (U9) | **nrf2\_CSN** | Chip Select |
| GPIO45 | 30 | nRF24L01 (U9) | **nrf2\_CE** | Chip Enable |
| **Keypad/Buttons (via Pull-Up Resistors)** | | | | |
| RTC\_GPIO14, GPIO14 | 20 | DPAD Buttons | **DPAD\_UP** | Input via R4 |
| RTC\_GPIO13, GPIO13 | 19 | DPAD Buttons | **DPAD\_DOWN** | Input via R3 |
| RTC\_GPIO12, GPIO12 | 18 | DPAD Buttons | **DPAD\_LEFT** | Input via R2 |
| RTC\_GPIO11, GPIO11 | 17 | DPAD Buttons | **DPAD\_RIGHT** | Input via R1 |
| RTC\_GPIO10, GPIO10 | 16 | X/Y Buttons | **Y** | Input via R8 |
| RTC\_GPIO9, GPIO9 | 15 | A/B Buttons | **B** | Input via R6 |
| RTC\_GPIO20, GPIO20 | 26 | Settings | **KEY\_SETTINGS** | Input via R9 |
| **Other Connections** | | | | |
| RX | 42 | UART | **RX** | UART Receive |
| TX | 43 | UART | **TX** | UART Transmit |
| RTC\_GPIO19, GPIO19 | 25 | Sound Header | **GPIO19** | Sound Output |

***

## Akira-Micro Pin Mapping

This device is built around the **ESP32** module (**U2**) and focuses on wireless communication with the **nRF24L01** (U1/U9) and **CC1101** modules, as well as an **SSD1306 OLED** and **MicroSD card**.

---

### 📍 ESP32 (U2) Main Pin Assignments

| ESP32 Pin (U2) | Pin Number | Peripheral/Component | Signal Name | Function |
| :---: | :---: | :---: | :---: | :---: |
| **I2C Bus (OLED)** | | | | |
| IO21 | 33 | OLED (P2) | **SDA** | I2C Data Line |
| IO22 | 36 | OLED (P2) | **SCL** | I2C Clock Line |
| **Shared SPI Bus (nRF24L01, CC1101, SD Card)** | | | | |
| IO5 | 29 | CC1101 Module | **MISO** | SPI Data Out |
| IO18 | 30 | CC1101 Module | **SCK** | SPI Clock |
| IO19 | 31 | CC1101 Module | **MOSI** | SPI Data In |
| **Chip Select & Enable Pins** | | | | |
| IO0 | 25 | SD Card (U4) | **CS\_SD** | Chip Select |
| IO23 | 37 | CC1101 Module | **CS\_CC** | Chip Select |
| IO17 | 28 | nRF24L01 (U9) | **CE\_1** | Chip Enable |
| IO16 | 27 | nRF24L01 (U1) | **CE\_2** | Chip Enable |
| **UART/Programming** | | | | |
| RXD0 | 34 | UART (P1) | **RX** | UART Receive (Connects to P1 TX) |
| TXD0 | 35 | UART (P1) | **TX** | UART Transmit (Connects to P1 RX) |
| **Keypad/Buttons (via Pull-Up Resistors)** | | | | |
| I/35 | 7 | DPAD Buttons | **KEY\_1** | Input |
| I/34 | 6 | DPAD Buttons | **KEY\_2** | Input |
| I/39 | 5 | DPAD Buttons | **KEY\_3** | Input |
| I/36 | 4 | DPAD Buttons | **KEY\_4** | Input |
| IO14 | 13 | DPAD Buttons | **KEY\_5** | Input |
| IO13 | 16 | DPAD Buttons | **KEY\_6** | Input |
| **Other Connections** | | | | |
| IO32 | 8 | LED | **STATUS (BLUE)** | Blue Status LED (via R2, 470Ω) |
| EN | 3 | Power Management | **EN** | Enable Pin (connected to RESET switch SW1) |


