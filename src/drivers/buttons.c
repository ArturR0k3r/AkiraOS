

#include "buttons.h"
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_DBG);

static const struct device *gpio_dev;
static gpio_pin_t button_pins[BUTTON_COUNT] = {
    PIN_BTN_ONOFF,    // BUTTON_ID_ONOFF
    PIN_BTN_SETTINGS, // BUTTON_ID_SETTINGS
    PIN_BTN_UP,       // BUTTON_ID_UP
    PIN_BTN_DOWN,     // BUTTON_ID_DOWN
    PIN_BTN_LEFT,     // BUTTON_ID_LEFT
    PIN_BTN_RIGHT,    // BUTTON_ID_RIGHT
    PIN_BTN_A,        // BUTTON_ID_A
    PIN_BTN_B,        // BUTTON_ID_B
    PIN_BTN_X,        // BUTTON_ID_X
    PIN_BTN_Y         // BUTTON_ID_Y
};

int buttons_init(void)
{
    gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio_dev))
        return -ENODEV;

    // Configure all pins as input (no pull-up for 34, 35, 39)
    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        int flags = GPIO_INPUT;
        if (button_pins[i] != 34 && button_pins[i] != 35 && button_pins[i] != 39)
            flags |= GPIO_PULL_UP;
        int ret = gpio_pin_configure(gpio_dev, button_pins[i], flags);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int button_is_pressed(int button_id)
{
    if (button_id < 0 || button_id >= BUTTON_COUNT)
        return 0;
    // Active low: pressed when reads 0
    return gpio_pin_get(gpio_dev, button_pins[button_id]) == 0;
}

void buttons_poll_events(button_event_cb_t cb)
{
    // Just call callback with current state for debug
    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        int pressed = button_is_pressed(i);
        cb(i, pressed ? BUTTON_EVENT_SINGLE_CLICK : BUTTON_EVENT_NONE);
    }
}

void buttons_debug_print_raw(void)
{
    char buf[128];
    int len = 0;
    len += snprintf(buf + len, sizeof(buf) - len, "RAW: ");
    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        int val = gpio_pin_get(gpio_dev, button_pins[i]);
        len += snprintf(buf + len, sizeof(buf) - len, "%d:%d ", i, val);
    }
    LOG_INF("%s", buf);
}
