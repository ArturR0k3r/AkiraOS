// buttons.h
#ifndef BUTTONS_H
#define BUTTONS_H

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

// Direct pin definitions
#define PIN_BTN_ONOFF 12    // 0
#define PIN_BTN_SETTINGS 16 // 1
#define PIN_BTN_UP 34       // 2 -
#define PIN_BTN_DOWN 33     // 3
#define PIN_BTN_LEFT 32     // 4
#define PIN_BTN_RIGHT 39    // 5 -
#define PIN_BTN_A 17        // 6
#define PIN_BTN_B 14        // 7
#define PIN_BTN_X 35        // 8 -
#define PIN_BTN_Y 13        // 9

typedef enum
{
    BUTTON_ID_ONOFF = 0,
    BUTTON_ID_SETTINGS,
    BUTTON_ID_UP,
    BUTTON_ID_DOWN,
    BUTTON_ID_LEFT,
    BUTTON_ID_RIGHT,
    BUTTON_ID_A,
    BUTTON_ID_B,
    BUTTON_ID_X,
    BUTTON_ID_Y,
    BUTTON_COUNT
} button_id_t;

typedef enum
{
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SINGLE_CLICK,
    BUTTON_EVENT_DOUBLE_CLICK,
    BUTTON_EVENT_TRIPLE_CLICK,
    BUTTON_EVENT_LONG_PRESS
} button_event_t;

typedef void (*button_event_cb_t)(int button_id, button_event_t event);

struct button
{
    gpio_pin_t pin;
    // Internal state for event detection
    uint32_t last_press_time;
    uint8_t click_count;
    bool pressed;
};

int buttons_init(void);
int button_is_pressed(int button_id);
void buttons_poll_events(button_event_cb_t cb);
void buttons_debug_print_raw(void);
#endif // BUTTONS_H
