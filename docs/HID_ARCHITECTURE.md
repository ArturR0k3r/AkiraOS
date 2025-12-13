# HID Architecture - Layered Design

## Overview
Clear separation between HID protocol, transports (BT/USB), and hardware drivers.

**Target Board:** Seeed XIAO nRF52840 (primary), ESP32-C3/ESP32-S3 (secondary)

**Key Features:**
- ✅ Rate limiting (125Hz max per USB HID spec)
- ✅ Configurable VID/PID from `prj.conf`
- ✅ Explicit capability-based security
- ✅ Modular layers (HID/BT/USB/Drivers/WASM)
- ✅ OCRE event system integration

```
┌─────────────────────────────────────────────────────────────┐
│                    WASM Applications                        │
│  (keypad_app.wasm, volume_control.wasm, macro_pad.wasm)   │
└────────────────────┬────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────────┐
│                 WASM API Layer                              │
│  • akira_hid_*     - HID control API                       │
│  • akira_bt_*      - Bluetooth control API                 │
│  • akira_usb_*     - USB control API                       │
│  • akira_button_*  - Button driver API                     │
│  • akira_encoder_* - Encoder driver API                    │
└──┬────────────┬────────────┬────────────┬──────────────┬───┘
   │            │            │            │              │
┌──▼─────┐  ┌──▼──────┐  ┌──▼──────┐  ┌─▼────────┐  ┌──▼──────┐
│  HID   │  │   BT    │  │   USB   │  │ Buttons  │  │Encoders │
│ Layer  │  │  Layer  │  │  Layer  │  │  Driver  │  │ Driver  │
└──┬─────┘  └──┬──────┘  └──┬──────┘  └─────┬────┘  └────┬────┘
   │           │            │               │            │
   │     ┌─────▼────────────▼───────┐       │            │
   └─────►   Transport Manager      │       │            │
         │  (routes HID reports)    │       │            │
         └──────────────────────────┘       │            │
                                             │            │
                                        ┌────▼────────────▼────┐
                                        │   Hardware (GPIO)    │
                                        └──────────────────────┘
```

---

## Layer 1: HID Protocol Layer

**Purpose:** Define HID reports and common protocol functions  
**Location:** `src/connectivity/hid/`

### Files:
- `hid_common.h` - HID types, keycodes, report structures ✅
- `hid_keyboard.c/h` - Keyboard profile implementation
- `hid_gamepad.c/h` - Gamepad profile implementation  
- `hid_mouse.c/h` - Mouse profile implementation (future)

### API:
```c
// Keyboard functions
int hid_keyboard_init(void);
int hid_keyboard_press_key(uint8_t keycode);
int hid_keyboard_release_key(uint8_t keycode);
int hid_keyboard_release_all(void);
int hid_keyboard_set_modifiers(uint8_t modifiers);
int hid_keyboard_get_report(hid_keyboard_report_t *report);
int hid_keyboard_send_throttled(void);  // ⬜ NEW: Rate-limited send

// Gamepad functions
int hid_gamepad_init(void);
int hid_gamepad_press_button(uint8_t button);
int hid_gamepad_release_button(uint8_t button);
int hid_gamepad_set_axis(hid_gamepad_axis_t axis, int16_t value);
int hid_gamepad_set_hat(int8_t direction);
int hid_gamepad_get_report(hid_gamepad_report_t *report);
int hid_gamepad_send_throttled(void);   // ⬜ NEW: Rate-limited send
```

**Rate Limiting:**
- Enforces USB HID spec limit: 125Hz max (8ms minimum interval)
- Prevents report spam on USB and BLE transports
- Returns `-EAGAIN` if called too soon (< 8ms since last report)
- Implementation: `HID_REPORT_INTERVAL_MS = 8` in `hid_manager.c`

**Report Structures:**
```c
typedef struct {
    uint8_t report_id;      // 0x01 for keyboard
    uint8_t modifiers;      // Ctrl, Shift, Alt, GUI
    uint8_t reserved;       // Always 0
    uint8_t keys[6];        // Up to 6 simultaneous keys
} hid_keyboard_report_t;

typedef struct {
    uint8_t report_id;      // 0x02 for gamepad
    int16_t axes[4];        // Left X/Y, Right X/Y (-32768 to 32767)
    int16_t triggers[2];    // L2/R2 triggers
    uint16_t buttons;       // 16 button bits
    int8_t hat;             // D-pad (0-7 or -1)
    uint8_t padding;        // Alignment
} hid_gamepad_report_t;
```

---

## Layer 2: Bluetooth Layer

**Purpose:** BLE stack, GATT services, connections  
**Location:** `src/connectivity/bluetooth/`

### Files:
- `bt_manager.c/h` - BT initialization, advertising, connections ✅
- `bt_hid_service.c/h` - GATT HIDS implementation (NEW)
- `bt_bas.c/h` - Battery Service (optional)
- `bt_dis.c/h` - Device Information Service (optional)

### API:
```c
// BT Manager (connection/advertising)
int bt_manager_init(const char *device_name);
int bt_manager_start_advertising(void);
int bt_manager_stop_advertising(void);
bool bt_manager_is_connected(void);
int bt_manager_disconnect(void);
int bt_manager_get_connection_info(bt_conn_info_t *info);

// BT HID Service (GATT operations)
int bt_hid_service_init(void);
int bt_hid_service_register(void);
int bt_hid_service_send_keyboard_report(const hid_keyboard_report_t *report);
int bt_hid_service_send_gamepad_report(const hid_gamepad_report_t *report);
int bt_hid_service_set_report_callback(bt_hid_output_cb callback);
```

**GATT Services to Implement:**
1. **HID Service (0x1812)**
   - HID Information (0x2A4A)
   - Report Map (0x2A4B) - Contains HID descriptors
   - HID Control Point (0x2A4C)
   - Protocol Mode (0x2A4E)
   - Input Report (0x2A4D) - Keyboard/Gamepad data
   - Output Report (0x2A4D) - LED status
   
2. **Battery Service (0x180F)** - Optional
   - Battery Level (0x2A19)

3. **Device Information Service (0x180A)** - Optional
   - Manufacturer Name (0x2A29)
   - Model Number (0x2A24)
   - PnP ID (0x2A50)

---

## Layer 3: USB Layer

**Purpose:** USB device stack, HID class  
**Location:** `src/connectivity/usb/`

### Files:
- `usb_manager.c/h` - USB initialization, device enumeration
- `usb_hid_device.c/h` - USB HID class implementation (NEW)

### API:
```c
// USB Manager
int usb_manager_init(const char *product_name);
int usb_manager_enable(void);
int usb_manager_disable(void);
bool usb_manager_is_configured(void);
int usb_manager_get_status(usb_status_t *status);

// USB HID Device
int usb_hid_device_init(void);
int usb_hid_device_register(void);
int usb_hid_device_send_keyboard_report(const hid_keyboard_report_t *report);
int usb_hid_device_send_gamepad_report(const hid_gamepad_report_t *report);
int usb_hid_device_set_report_callback(usb_hid_output_cb callback);
```

**USB HID Class:**
- Implement USB HID Boot Protocol + Report Protocol
- Register HID descriptors (keyboard, gamepad)
- Handle SET_REPORT, GET_REPORT, SET_IDLE, GET_IDLE
- Interrupt IN endpoint for sending reports
- Interrupt OUT endpoint for receiving LED status

---

## Layer 4: Hardware Drivers

**Purpose:** Direct hardware control (buttons, encoders)  
**Location:** `src/drivers/`

### Files:
- `akira_buttons.c/h` - Button driver with interrupts ✅ (enhance)
- `akira_encoder.c/h` - Rotary encoder driver (NEW)

### Button Driver API:
```c
typedef enum {
    BUTTON_EVENT_PRESSED,
    BUTTON_EVENT_RELEASED,
    BUTTON_EVENT_LONG_PRESS,
    BUTTON_EVENT_DOUBLE_CLICK
} button_event_t;

typedef void (*button_callback_t)(uint8_t button_id, button_event_t event, void *user_data);

// Init/deinit
int akira_buttons_init(void);
int akira_buttons_deinit(void);

// Polling API
uint16_t akira_buttons_get_state(void);
bool akira_buttons_is_pressed(uint8_t button_id);

// Callback API
int akira_buttons_register_callback(uint8_t button_id, button_callback_t cb, void *user_data);
int akira_buttons_unregister_callback(uint8_t button_id);

// Configuration
int akira_buttons_set_debounce_ms(uint32_t ms);
int akira_buttons_set_long_press_ms(uint32_t ms);
int akira_buttons_set_double_click_ms(uint32_t ms);
```

### Encoder Driver API:
```c
typedef enum {
    ENCODER_EVENT_CW,               // Clockwise
    ENCODER_EVENT_CCW,              // Counter-clockwise
    ENCODER_EVENT_BUTTON_PRESS,     // Encoder button
    ENCODER_EVENT_BUTTON_RELEASE
} encoder_event_t;

typedef void (*encoder_callback_t)(uint8_t encoder_id, encoder_event_t event, int32_t steps, void *user_data);

typedef struct {
    uint8_t pin_a;              // CLK
    uint8_t pin_b;              // DT
    uint8_t pin_button;         // SW (optional, -1 if unused)
    bool invert_direction;
    uint8_t steps_per_detent;   // Usually 4
} encoder_config_t;

// Init/deinit
int akira_encoder_init(void);
int akira_encoder_deinit(void);

// Instance management
int akira_encoder_add(uint8_t encoder_id, const encoder_config_t *config);
int akira_encoder_remove(uint8_t encoder_id);

// Position API
int32_t akira_encoder_get_position(uint8_t encoder_id);
int akira_encoder_reset_position(uint8_t encoder_id);

// Callback API
int akira_encoder_register_callback(uint8_t encoder_id, encoder_callback_t cb, void *user_data);
int akira_encoder_unregister_callback(uint8_t encoder_id);
```

---

## Layer 5: WASM API Layer

**Purpose:** Expose all functions to WASM applications  
**Location:** `src/api/`

### Files:
- `wasm_hid_api.c/h` - HID protocol functions
- `wasm_bt_api.c/h` - Bluetooth control functions
- `wasm_usb_api.c/h` - USB control functions
- `wasm_button_api.c/h` - Button driver functions
- `wasm_encoder_api.c/h` - Encoder driver functions

### WASM HID API:
```c
NativeSymbol wasm_hid_api[] = {
    // Keyboard
    {"akira_hid_keyboard_init", wasm_hid_keyboard_init, "()i", NULL},
    {"akira_hid_keyboard_press", wasm_hid_keyboard_press, "(i)i", NULL},
    {"akira_hid_keyboard_release", wasm_hid_keyboard_release, "(i)i", NULL},
    {"akira_hid_keyboard_type_string", wasm_hid_keyboard_type_string, "($)i", NULL},
    {"akira_hid_keyboard_release_all", wasm_hid_keyboard_release_all, "()i", NULL},
    
    // Gamepad
    {"akira_hid_gamepad_init", wasm_hid_gamepad_init, "()i", NULL},
    {"akira_hid_gamepad_button_press", wasm_hid_gamepad_button_press, "(i)i", NULL},
    {"akira_hid_gamepad_button_release", wasm_hid_gamepad_button_release, "(i)i", NULL},
    {"akira_hid_gamepad_set_axis", wasm_hid_gamepad_set_axis, "(ii)i", NULL},
    {"akira_hid_gamepad_set_hat", wasm_hid_gamepad_set_hat, "(i)i", NULL},
};
```

### WASM BT API:
```c
NativeSymbol wasm_bt_api[] = {
    {"akira_bt_init", wasm_bt_init, "($)i", NULL},  // device_name
    {"akira_bt_start_advertising", wasm_bt_start_advertising, "()i", NULL},
    {"akira_bt_stop_advertising", wasm_bt_stop_advertising, "()i", NULL},
    {"akira_bt_is_connected", wasm_bt_is_connected, "()i", NULL},
    {"akira_bt_disconnect", wasm_bt_disconnect, "()i", NULL},
    {"akira_bt_hid_enable", wasm_bt_hid_enable, "()i", NULL},
    {"akira_bt_hid_send_keyboard", wasm_bt_hid_send_keyboard, "()i", NULL},  // Uses current report
    {"akira_bt_hid_send_gamepad", wasm_bt_hid_send_gamepad, "()i", NULL},
};
```

### WASM USB API:
```c
NativeSymbol wasm_usb_api[] = {
    {"akira_usb_init", wasm_usb_init, "($)i", NULL},  // product_name
    {"akira_usb_enable", wasm_usb_enable, "()i", NULL},
    {"akira_usb_disable", wasm_usb_disable, "()i", NULL},
    {"akira_usb_is_configured", wasm_usb_is_configured, "()i", NULL},
    {"akira_usb_hid_enable", wasm_usb_hid_enable, "()i", NULL},
    {"akira_usb_hid_send_keyboard", wasm_usb_hid_send_keyboard, "()i", NULL},
    {"akira_usb_hid_send_gamepad", wasm_usb_hid_send_gamepad, "()i", NULL},
};
```

### WASM Button API:
```c
NativeSymbol wasm_button_api[] = {
    {"akira_button_init", wasm_button_init, "()i", NULL},
    {"akira_button_get_state", wasm_button_get_state, "()i", NULL},  // Returns bitmask
    {"akira_button_is_pressed", wasm_button_is_pressed, "(i)i", NULL},
    {"akira_button_register_callback", wasm_button_register_callback, "(i)i", NULL},
    {"akira_button_set_long_press_ms", wasm_button_set_long_press_ms, "(i)i", NULL},
};
```

### WASM Encoder API:
```c
NativeSymbol wasm_encoder_api[] = {
    {"akira_encoder_init", wasm_encoder_init, "()i", NULL},
    {"akira_encoder_add", wasm_encoder_add, "(iiiii)i", NULL},  // id, pin_a, pin_b, pin_btn, invert
    {"akira_encoder_get_position", wasm_encoder_get_position, "(i)i", NULL},
    {"akira_encoder_reset_position", wasm_encoder_reset_position, "(i)i", NULL},
    {"akira_encoder_register_callback", wasm_encoder_register_callback, "(i)i", NULL},
};
```

---

## Security & Capabilities

### WASM Capability System

Apps must declare required capabilities in their manifest:

```c
// src/security/capability.c
typedef enum {
    // Existing capabilities...
    CAP_DISPLAY,
    CAP_NETWORK,
    CAP_SENSOR,
    
    // HID capabilities (NEW):
    CAP_HID_KEYBOARD,        // Can send keyboard input
    CAP_HID_GAMEPAD,         // Can send gamepad input
    CAP_HID_MOUSE,           // Future: Can send mouse input
    CAP_BLUETOOTH_CONTROL,   // Can control BT stack
    CAP_USB_CONTROL,         // Can control USB stack
    CAP_INPUT_DEVICES        // Can access buttons/encoders
} capability_t;
```

### App Manifest Example

```json
{
  "name": "ble_keyboard",
  "version": "1.0.0",
  "permissions": [
    "hid_keyboard",
    "bluetooth_control",
    "input_devices"
  ]
}
```

### Capability Enforcement

- WASM API functions check capabilities before execution
- Apps without `CAP_BLUETOOTH_CONTROL` cannot call `akira_bt_*` functions
- Apps without `CAP_USB_CONTROL` cannot call `akira_usb_*` functions
- Apps without `CAP_INPUT_DEVICES` cannot access buttons/encoders
- Follows existing OCRE security model

---

## Implementation Order

### Phase 1: HID Protocol Layer
1. ✅ `hid_common.h` (already exists)
2. ⬜ `hid_keyboard.c/h` - Keyboard report management
3. ⬜ `hid_gamepad.c/h` - Gamepad report management

### Phase 2: Bluetooth Layer
4. ✅ `bt_manager.c/h` (already exists)
5. ⬜ `bt_hid_service.c/h` - GATT HIDS implementation

### Phase 3: USB Layer
6. ⬜ `usb_manager.c/h` - USB device initialization
7. ⬜ `usb_hid_device.c/h` - USB HID class

### Phase 4: Hardware Drivers
8. ✅ `akira_buttons.c/h` (exists, needs callbacks)
9. ⬜ `akira_encoder.c/h` - Rotary encoder driver

### Phase 5: WASM API
10. ⬜ `wasm_hid_api.c/h`
11. ⬜ `wasm_bt_api.c/h`
12. ⬜ `wasm_usb_api.c/h`
13. ⬜ `wasm_button_api.c/h`
14. ⬜ `wasm_encoder_api.c/h`

---

## Kconfig Structure

```kconfig
menu "AkiraOS HID System"

config AKIRA_HID
    bool "Enable HID Protocol Layer"
    default n
    help
      Common HID protocol implementations (keyboard, gamepad)

config AKIRA_HID_KEYBOARD
    bool "Enable HID Keyboard Profile"
    depends on AKIRA_HID
    default y

config AKIRA_HID_GAMEPAD
    bool "HID Gamepad Profile"
    depends on AKIRA_HID
    default y

config AKIRA_HID_VID
    hex "USB/BLE Vendor ID (VID)"
    default 0x1209
    depends on AKIRA_HID

config AKIRA_HID_PID
    hex "USB/BLE Product ID (PID)"
    default 0x0001
    depends on AKIRA_HID

config AKIRA_HID_MANUFACTURER
    string "HID Manufacturer Name"
    default "AkiraOS"
    depends on AKIRA_HID

config AKIRA_HID_PRODUCT
    string "HID Product Name"
    default "Akira HID Device"
    depends on AKIRA_HID

config AKIRA_HID_SERIAL
    string "HID Serial Number"
    default "123456"
    depends on AKIRA_HID

endmenu

menu "AkiraOS Bluetooth"

config AKIRA_BT
    bool "Enable Bluetooth Manager"
    depends on BT
    default n

config AKIRA_BT_HID
    bool "Enable Bluetooth HID Service"
    depends on AKIRA_BT && AKIRA_HID
    select BT_GATT_HIDS
    default n

endmenu

menu "AkiraOS USB"

config AKIRA_USB
    bool "Enable USB Manager"
    depends on USB_DEVICE_STACK
    default n

config AKIRA_USB_HID
    bool "Enable USB HID Device"
    depends on AKIRA_USB && AKIRA_HID
    select USB_DEVICE_HID
    default n

endmenu

menu "AkiraOS Input Drivers"

config AKIRA_DRIVER_BUTTONS
    bool "Enable Button Driver"
    default y

config AKIRA_DRIVER_ENCODERS
    bool "Enable Rotary Encoder Driver"
    default n

endmenu

menu "AkiraOS WASM APIs"

config AKIRA_WASM_HID_API
    bool "WASM HID API"
    depends on AKIRA_HID && AKIRA_APP_MANAGER
    default n

config AKIRA_WASM_BT_API
    bool "WASM Bluetooth API"
    depends on AKIRA_BT && AKIRA_APP_MANAGER
    default n

config AKIRA_WASM_USB_API
    bool "WASM USB API"
    depends on AKIRA_USB && AKIRA_APP_MANAGER
    default n

config AKIRA_WASM_BUTTON_API
    bool "WASM Button API"
    depends on AKIRA_DRIVER_BUTTONS && AKIRA_APP_MANAGER
    default n

config AKIRA_WASM_ENCODER_API
    bool "WASM Encoder API"
    depends on AKIRA_DRIVER_ENCODERS && AKIRA_APP_MANAGER
    default n

endmenu
```

---

## Example Usage

### WASM App: BLE Keyboard
```c
#include "akira_api.h"

void on_init() {
    // Initialize HID protocol
    akira_hid_keyboard_init();
    
    // Initialize Bluetooth
    akira_bt_init("AkiraKeyboard");
    akira_bt_hid_enable();
    akira_bt_start_advertising();
    
    // Initialize button driver
    akira_button_init();
    akira_button_register_callback(6);  // BTN_A
}

void on_timer() {
    // Check if BT connected
    if (!akira_bt_is_connected()) return;
    
    // Check button state
    if (akira_button_is_pressed(6)) {  // BTN_A
        akira_hid_keyboard_press(0x04);  // 'A'
        akira_bt_hid_send_keyboard();
    } else {
        akira_hid_keyboard_release(0x04);
        akira_bt_hid_send_keyboard();
    }
}
```

### WASM App: USB Gamepad
```c
void on_init() {
    akira_hid_gamepad_init();
    akira_usb_init("AkiraGamepad");
    akira_usb_hid_enable();
    akira_usb_enable();
    
    akira_button_init();
}

void on_timer() {
    if (!akira_usb_is_configured()) return;
    
    // Map buttons to gamepad
    if (akira_button_is_pressed(6)) {  // BTN_A -> Gamepad A
        akira_hid_gamepad_button_press(0);
    } else {
        akira_hid_gamepad_button_release(0);
    }
    
    akira_usb_hid_send_gamepad();
}
```

---

This architecture provides **clean separation** of concerns with independent layers that can be used standalone or combined.
