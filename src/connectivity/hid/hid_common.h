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

#ifdef __cplusplus
extern "C"
{
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

    /** Keyboard modifier keys (matches USB HID spec) */
    typedef enum
    {
        HID_MOD_NONE = 0x00,
        HID_MOD_LEFT_CTRL = 0x01,
        HID_MOD_LEFT_SHIFT = 0x02,
        HID_MOD_LEFT_ALT = 0x04,
        HID_MOD_LEFT_GUI = 0x08,
        HID_MOD_RIGHT_CTRL = 0x10,
        HID_MOD_RIGHT_SHIFT = 0x20,
        HID_MOD_RIGHT_ALT = 0x40,
        HID_MOD_RIGHT_GUI = 0x80
    } hid_keyboard_mod_t;

    /** Common keyboard key codes (USB HID Usage Page 0x07) */
    typedef enum
    {
        HID_KEY_NONE = 0x00,
        HID_KEY_A = 0x04,
        HID_KEY_B = 0x05,
        HID_KEY_C = 0x06,
        HID_KEY_D = 0x07,
        HID_KEY_E = 0x08,
        HID_KEY_F = 0x09,
        HID_KEY_G = 0x0A,
        HID_KEY_H = 0x0B,
        HID_KEY_I = 0x0C,
        HID_KEY_J = 0x0D,
        HID_KEY_K = 0x0E,
        HID_KEY_L = 0x0F,
        HID_KEY_M = 0x10,
        HID_KEY_N = 0x11,
        HID_KEY_O = 0x12,
        HID_KEY_P = 0x13,
        HID_KEY_Q = 0x14,
        HID_KEY_R = 0x15,
        HID_KEY_S = 0x16,
        HID_KEY_T = 0x17,
        HID_KEY_U = 0x18,
        HID_KEY_V = 0x19,
        HID_KEY_W = 0x1A,
        HID_KEY_X = 0x1B,
        HID_KEY_Y = 0x1C,
        HID_KEY_Z = 0x1D,
        HID_KEY_1 = 0x1E,
        HID_KEY_2 = 0x1F,
        HID_KEY_3 = 0x20,
        HID_KEY_4 = 0x21,
        HID_KEY_5 = 0x22,
        HID_KEY_6 = 0x23,
        HID_KEY_7 = 0x24,
        HID_KEY_8 = 0x25,
        HID_KEY_9 = 0x26,
        HID_KEY_0 = 0x27,
        HID_KEY_ENTER = 0x28,
        HID_KEY_ESC = 0x29,
        HID_KEY_BACKSPACE = 0x2A,
        HID_KEY_TAB = 0x2B,
        HID_KEY_SPACE = 0x2C,
        HID_KEY_MINUS = 0x2D,
        HID_KEY_EQUAL = 0x2E,
        HID_KEY_LEFT_BRACE = 0x2F,
        HID_KEY_RIGHT_BRACE = 0x30,
        HID_KEY_BACKSLASH = 0x31,
        HID_KEY_SEMICOLON = 0x33,
        HID_KEY_QUOTE = 0x34,
        HID_KEY_GRAVE = 0x35,
        HID_KEY_COMMA = 0x36,
        HID_KEY_DOT = 0x37,
        HID_KEY_SLASH = 0x38,
        HID_KEY_CAPS_LOCK = 0x39,
        HID_KEY_F1 = 0x3A,
        HID_KEY_F2 = 0x3B,
        HID_KEY_F3 = 0x3C,
        HID_KEY_F4 = 0x3D,
        HID_KEY_F5 = 0x3E,
        HID_KEY_F6 = 0x3F,
        HID_KEY_F7 = 0x40,
        HID_KEY_F8 = 0x41,
        HID_KEY_F9 = 0x42,
        HID_KEY_F10 = 0x43,
        HID_KEY_F11 = 0x44,
        HID_KEY_F12 = 0x45,
        HID_KEY_UP = 0x52,
        HID_KEY_DOWN = 0x51,
        HID_KEY_LEFT = 0x50,
        HID_KEY_RIGHT = 0x4F
    } hid_key_code_t;

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
