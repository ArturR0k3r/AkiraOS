/**
 * @file hid_common.h
 * @brief Common HID Definitions for AkiraOS
 *
 * Provides unified HID types for keyboard and gamepad profiles
 * used by both Bluetooth HID and USB HID implementations.
 */

#ifndef AKIRA_HID_COMMON_H
#define AKIRA_HID_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Use Zephyr's HID key definitions when available */
#if defined(CONFIG_USB_DEVICE_STACK) || defined(CONFIG_BT)
#include <zephyr/usb/class/hid.h>

/* Map Zephyr's naming to our naming convention */
#define HID_KEY_NONE 0x00
#define HID_KEY_LEFT_BRACE HID_KEY_LEFTBRACE
#define HID_KEY_RIGHT_BRACE HID_KEY_RIGHTBRACE
#define HID_KEY_QUOTE HID_KEY_APOSTROPHE
#define HID_KEY_CAPS_LOCK HID_KEY_CAPSLOCK

#else
/* Define minimal HID key codes for non-USB/BT builds */
#define HID_KEY_NONE 0x00
#define HID_KEY_A 0x04
#define HID_KEY_LEFT_BRACE 0x2F
#define HID_KEY_RIGHT_BRACE 0x30
#define HID_KEY_QUOTE 0x34
#define HID_KEY_CAPS_LOCK 0x39
/* Add more as needed */
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/*===========================================================================*/
/* HID Key Codes (Using Zephyr definitions) */
/*===========================================================================*/

/* Zephyr provides HID_KEY_* constants, use those directly */
typedef uint8_t hid_key_code_t;

/*===========================================================================*/
/* HID Modifier Keys (Using Zephyr definitions when available) */
/*===========================================================================*/

#if defined(CONFIG_USB_DEVICE_STACK) || defined(CONFIG_BT)
/* Zephyr provides HID_KBD_MODIFIER_* constants */
#define HID_MOD_LEFT_CTRL   HID_KBD_MODIFIER_LEFT_CTRL
#define HID_MOD_LEFT_SHIFT  HID_KBD_MODIFIER_LEFT_SHIFT
#define HID_MOD_LEFT_ALT    HID_KBD_MODIFIER_LEFT_ALT
#define HID_MOD_LEFT_GUI    HID_KBD_MODIFIER_LEFT_UI
#define HID_MOD_RIGHT_CTRL  HID_KBD_MODIFIER_RIGHT_CTRL
#define HID_MOD_RIGHT_SHIFT HID_KBD_MODIFIER_RIGHT_SHIFT
#define HID_MOD_RIGHT_ALT   HID_KBD_MODIFIER_RIGHT_ALT
#define HID_MOD_RIGHT_GUI   HID_KBD_MODIFIER_RIGHT_UI
#else
/* Fallback definitions */
#define HID_MOD_LEFT_CTRL   0x01
#define HID_MOD_LEFT_SHIFT  0x02
#define HID_MOD_LEFT_ALT    0x04
#define HID_MOD_LEFT_GUI    0x08
#define HID_MOD_RIGHT_CTRL  0x10
#define HID_MOD_RIGHT_SHIFT 0x20
#define HID_MOD_RIGHT_ALT   0x40
#define HID_MOD_RIGHT_GUI   0x80
#endif

/*===========================================================================*/
/* HID Configuration                                                         */
/*===========================================================================*/

/** Maximum simultaneous key presses for keyboard */
#define HID_MAX_KEYS 6

/** Gamepad button count */
#define HID_GAMEPAD_MAX_BUTTONS 16

/** Gamepad axis count (2 sticks = 4 axes + triggers = 6) */
#define HID_GAMEPAD_MAX_AXES 6

/*===========================================================================*/
/* HID Transport Types                                                       */
/*===========================================================================*/

/** HID transport type */
typedef enum
{
    HID_TRANSPORT_NONE = 0,
    HID_TRANSPORT_BLE,
    HID_TRANSPORT_USB,
    HID_TRANSPORT_SIMULATED
} hid_transport_t;

/** HID device type */
typedef enum
{
    HID_DEVICE_KEYBOARD = 0x01,
    HID_DEVICE_GAMEPAD = 0x02,
    HID_DEVICE_MOUSE = 0x04,
    HID_DEVICE_COMBO = 0x07 /* All of the above */
} hid_device_type_t;

/*===========================================================================*/
/* Keyboard HID Definitions                                                  */
/*===========================================================================*/

/* Note: Keyboard key codes (HID_KEY_*) come from Zephyr's hid.h */
/* Note: Keyboard modifier defines (HID_MOD_*) mapped above */

/** Keyboard report structure */
typedef struct
{
    uint8_t modifiers;          /**< Modifier keys bitmask */
    uint8_t reserved;           /**< Reserved byte */
    uint8_t keys[HID_MAX_KEYS]; /**< Currently pressed keys */
} __attribute__((packed)) hid_keyboard_report_t;

/*===========================================================================*/
/* Gamepad HID Definitions                                                   */
/*===========================================================================*/

/** Gamepad buttons (matches common USB gamepad layout) */
typedef enum
{
    HID_GAMEPAD_BTN_A = 0x0001,
    HID_GAMEPAD_BTN_B = 0x0002,
    HID_GAMEPAD_BTN_X = 0x0004,
    HID_GAMEPAD_BTN_Y = 0x0008,
    HID_GAMEPAD_BTN_LB = 0x0010,   /* Left bumper */
    HID_GAMEPAD_BTN_RB = 0x0020,   /* Right bumper */
    HID_GAMEPAD_BTN_BACK = 0x0040, /* Select/Back */
    HID_GAMEPAD_BTN_START = 0x0080,
    HID_GAMEPAD_BTN_HOME = 0x0100, /* Guide/Home */
    HID_GAMEPAD_BTN_L3 = 0x0200,   /* Left stick press */
    HID_GAMEPAD_BTN_R3 = 0x0400,   /* Right stick press */
    HID_GAMEPAD_DPAD_UP = 0x1000,
    HID_GAMEPAD_DPAD_DOWN = 0x2000,
    HID_GAMEPAD_DPAD_LEFT = 0x4000,
    HID_GAMEPAD_DPAD_RIGHT = 0x8000
} hid_gamepad_btn_t;

/** Gamepad axis indices */
typedef enum
{
    HID_AXIS_LEFT_X = 0,
    HID_AXIS_LEFT_Y = 1,
    HID_AXIS_RIGHT_X = 2,
    HID_AXIS_RIGHT_Y = 3,
    HID_AXIS_LT = 4, /* Left trigger */
    HID_AXIS_RT = 5  /* Right trigger */
} hid_gamepad_axis_t;

/** Gamepad report structure */
typedef struct
{
    int16_t axes[HID_GAMEPAD_MAX_AXES]; /**< Axis values (-32768 to 32767) */
    uint16_t buttons;                   /**< Button bitmask */
    uint8_t hat;                        /**< D-pad/Hat switch (0-8, 0=center) */
    uint8_t reserved;
} __attribute__((packed)) hid_gamepad_report_t;
    /*===========================================================================*/
    /* HID State Structure                                                       */
    /*===========================================================================*/

    /** Combined HID device state */
    typedef struct
    {
        hid_transport_t transport;
        hid_device_type_t device_type;
        bool connected;
        bool enabled;

        /* Reports */
        hid_keyboard_report_t keyboard;
        hid_gamepad_report_t gamepad;

        /* Statistics */
        uint32_t reports_sent;
        uint32_t errors;
    } hid_state_t;

    /*===========================================================================*/
    /* HID Callbacks                                                             */
    /*===========================================================================*/

    /** HID event types */
    typedef enum
    {
        HID_EVENT_CONNECTED,
        HID_EVENT_DISCONNECTED,
        HID_EVENT_SUSPENDED,
        HID_EVENT_RESUMED,
        HID_EVENT_OUTPUT_REPORT /* Host sent data (e.g., LED state) */
    } hid_event_t;

    /** HID event callback */
    typedef void (*hid_event_callback_t)(hid_event_t event, void *data, void *user_data);

    /** HID output report callback (for LED state, etc.) */
    typedef void (*hid_output_callback_t)(const uint8_t *data, size_t len, void *user_data);

    /*===========================================================================*/
    /* HID Interface (implemented by transports)                                 */
    /*===========================================================================*/

    /** HID transport operations */
    typedef struct
    {
        const char *name;

        int (*init)(hid_device_type_t types);
        int (*deinit)(void);
        int (*enable)(void);
        int (*disable)(void);

        /* Keyboard operations */
        int (*send_keyboard)(const hid_keyboard_report_t *report);

        /* Gamepad operations */
        int (*send_gamepad)(const hid_gamepad_report_t *report);

        /* Callbacks */
        int (*register_event_cb)(hid_event_callback_t cb, void *user_data);
        int (*register_output_cb)(hid_output_callback_t cb, void *user_data);

        /* State */
        bool (*is_connected)(void);
    } hid_transport_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_HID_COMMON_H */
