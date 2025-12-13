/**
 * @file akira_buttons.h
 * @brief Enhanced Button Driver for AkiraOS
 *
 * Features:
 * - GPIO interrupt-based detection
 * - Software debouncing
 * - Long press detection
 * - Event callbacks
 * - Button state tracking
 */

#ifndef AKIRA_BUTTONS_H
#define AKIRA_BUTTONS_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Button identifiers
 */
typedef enum {
    BUTTON_ONOFF = 0,
    BUTTON_SETTINGS = 1,
    BUTTON_UP = 2,
    BUTTON_DOWN = 3,
    BUTTON_LEFT = 4,
    BUTTON_RIGHT = 5,
    BUTTON_A = 6,
    BUTTON_B = 7,
    BUTTON_X = 8,
    BUTTON_Y = 9,
    BUTTON_COUNT
} button_id_t;

/**
 * Button event types
 */
typedef enum {
    BUTTON_EVENT_PRESS,      /* Button pressed */
    BUTTON_EVENT_RELEASE,    /* Button released */
    BUTTON_EVENT_HOLD,       /* Button held for long_press_ms */
    BUTTON_EVENT_CLICK,      /* Quick press and release */
} button_event_type_t;

/**
 * Button event structure
 */
typedef struct {
    button_id_t button;
    button_event_type_t type;
    uint32_t duration_ms;    /* How long button was held (for RELEASE/HOLD) */
    uint32_t timestamp;      /* Event timestamp (ms since boot) */
} button_event_t;

/**
 * Button event callback
 * @param event Button event data
 * @param user_data User-provided context
 */
typedef void (*button_event_callback_t)(const button_event_t *event, void *user_data);

/**
 * Button configuration
 */
typedef struct {
    uint32_t debounce_ms;        /* Debounce time (default: 20ms) */
    uint32_t long_press_ms;      /* Long press threshold (default: 1000ms) */
    bool repeat_enabled;         /* Enable key repeat for held buttons */
    uint32_t repeat_delay_ms;    /* Initial repeat delay (default: 500ms) */
    uint32_t repeat_interval_ms; /* Repeat interval (default: 100ms) */
} button_config_t;

/**
 * Initialize button driver
 * @param config Button configuration (NULL for defaults)
 * @return 0 on success, negative errno on error
 */
int akira_buttons_init(const button_config_t *config);

/**
 * Deinitialize button driver
 * @return 0 on success, negative errno on error
 */
int akira_buttons_deinit(void);

/**
 * Register event callback
 * @param callback Callback function
 * @param user_data User context passed to callback
 * @return 0 on success, negative errno on error
 */
int akira_buttons_register_callback(button_event_callback_t callback, void *user_data);

/**
 * Get current button states as bitmask
 * @return Bitmask of currently pressed buttons
 */
uint16_t akira_buttons_get_state(void);

/**
 * Check if specific button is pressed
 * @param button Button ID
 * @return true if pressed, false otherwise
 */
bool akira_buttons_is_pressed(button_id_t button);

/**
 * Get how long button has been held (ms)
 * @param button Button ID
 * @return Duration in milliseconds (0 if not pressed)
 */
uint32_t akira_buttons_get_hold_time(button_id_t button);

/* Button bitmask definitions for compatibility */
#define BTN_ONOFF (1 << BUTTON_ONOFF)
#define BTN_SETTINGS (1 << BUTTON_SETTINGS)
#define BTN_UP (1 << BUTTON_UP)
#define BTN_DOWN (1 << BUTTON_DOWN)
#define BTN_LEFT (1 << BUTTON_LEFT)
#define BTN_RIGHT (1 << BUTTON_RIGHT)
#define BTN_A (1 << BUTTON_A)
#define BTN_B (1 << BUTTON_B)
#define BTN_X (1 << BUTTON_X)
#define BTN_Y (1 << BUTTON_Y)

#endif /* AKIRA_BUTTONS_H */
