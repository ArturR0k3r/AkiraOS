/**
 * @file lvgl_input_driver.c
 * @brief LVGL input driver for touch/buttons
 * 
 * Provides touch and button input to LVGL.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_LVGL)
#include <lvgl.h>
#if defined(CONFIG_AKIRA_LVGL_INPUT_ZEPHYR)
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/sys/util.h>
#endif

LOG_MODULE_REGISTER(lvgl_input, LOG_LEVEL_INF);

static lv_indev_drv_t indev_drv_touch;
static lv_indev_drv_t indev_drv_buttons;
static lv_indev_t *indev_touch;
static lv_indev_t *indev_buttons;

/* Touch state */
static struct {
    int16_t x;
    int16_t y;
    bool pressed;
} touch_state = {0};

/* Button state */
static uint32_t button_state = 0;

#if defined(CONFIG_AKIRA_LVGL_INPUT_ZEPHYR)
/*
 * AkiraConsole gpio-keys "zephyr,code" values (see boards/*.overlay). Kept
 * local so this display driver does not pull in the WASM SDK headers.
 */
#define AKIRA_KEY_UP    2
#define AKIRA_KEY_DOWN  3
#define AKIRA_KEY_LEFT  4
#define AKIRA_KEY_RIGHT 5
#define AKIRA_KEY_A     6
#define AKIRA_KEY_B     9

/* Local keypad bitmask bit positions consumed by lvgl_button_read_cb(). */
#define BTN_BIT_UP    0
#define BTN_BIT_DOWN  1
#define BTN_BIT_LEFT  2
#define BTN_BIT_RIGHT 3
#define BTN_BIT_ENTER 4
#define BTN_BIT_ESC   5

static inline void set_button_bit(uint8_t bit, bool pressed)
{
    if (pressed) {
        button_state |= BIT(bit);
    } else {
        button_state &= ~BIT(bit);
    }
}

/**
 * @brief Zephyr input subsystem event handler
 *
 * Bridges the project's real input devices (gpio-keys D-pad / face buttons,
 * touch controllers, akira_pwm_dial, native_sim input, ...) into the LVGL
 * indev state. Registered for every device (dev == NULL) so the same LVGL
 * front-end works across boards without hardcoding a controller. Button
 * codes accept both the AkiraConsole gpio-keys mapping (zephyr,code 2..9)
 * and the standard Linux key / D-pad / gamepad codes for portability.
 */
static void lvgl_input_zephyr_cb(struct input_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);

    switch (evt->type) {
    case INPUT_EV_ABS:
        /* Absolute pointer axes from a touch / pen controller. */
        if (evt->code == INPUT_ABS_X) {
            touch_state.x = (int16_t)evt->value;
        } else if (evt->code == INPUT_ABS_Y) {
            touch_state.y = (int16_t)evt->value;
        }
        break;

    case INPUT_EV_KEY: {
        const bool pressed = (evt->value != 0);

        switch (evt->code) {
        case INPUT_BTN_TOUCH:
            touch_state.pressed = pressed;
            break;

        case AKIRA_KEY_UP:
        case INPUT_KEY_UP:
        case INPUT_BTN_DPAD_UP:
            set_button_bit(BTN_BIT_UP, pressed);
            break;
        case AKIRA_KEY_DOWN:
        case INPUT_KEY_DOWN:
        case INPUT_BTN_DPAD_DOWN:
            set_button_bit(BTN_BIT_DOWN, pressed);
            break;
        case AKIRA_KEY_LEFT:
        case INPUT_KEY_LEFT:
        case INPUT_BTN_DPAD_LEFT:
            set_button_bit(BTN_BIT_LEFT, pressed);
            break;
        case AKIRA_KEY_RIGHT:
        case INPUT_KEY_RIGHT:
        case INPUT_BTN_DPAD_RIGHT:
            set_button_bit(BTN_BIT_RIGHT, pressed);
            break;
        case AKIRA_KEY_A:      /* A / South / Enter -> LV_KEY_ENTER (select) */
        case INPUT_KEY_ENTER:
        case INPUT_BTN_SOUTH:
            set_button_bit(BTN_BIT_ENTER, pressed);
            break;
        case AKIRA_KEY_B:      /* B / East / Esc -> LV_KEY_ESC (back) */
        case INPUT_KEY_ESC:
        case INPUT_BTN_EAST:
            set_button_bit(BTN_BIT_ESC, pressed);
            break;
        default:
            /* Unmapped code (e.g. X/Y face buttons) — ignore. */
            break;
        }
        break;
    }

    default:
        break;
    }
}

/* Receive events from every input device in the system (dev == NULL). */
INPUT_CALLBACK_DEFINE(NULL, lvgl_input_zephyr_cb, NULL);
#endif /* CONFIG_AKIRA_LVGL_INPUT_ZEPHYR */

/**
 * @brief Touch input read callback
 * 
 * @param drv Input device driver
 * @param data Input data to fill
 */
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    /*
     * Touch state is populated by the Zephyr input handler
     * (lvgl_input_zephyr_cb) when CONFIG_AKIRA_LVGL_INPUT_ZEPHYR is enabled,
     * or by lvgl_input_update_touch() when a bespoke touch driver pushes data.
     */
    if (touch_state.pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = touch_state.x;
        data->point.y = touch_state.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/**
 * @brief Button input read callback
 * 
 * @param drv Input device driver
 * @param data Input data to fill
 */
static void lvgl_button_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    /*
     * Button state is maintained by the Zephyr input handler
     * (lvgl_input_zephyr_cb) when CONFIG_AKIRA_LVGL_INPUT_ZEPHYR is enabled,
     * or by lvgl_input_update_buttons() for a custom button driver.
     * Map the local button bitmask to LVGL key codes.
     */
    static uint32_t last_key = 0;

    if (button_state != 0) {
        data->state = LV_INDEV_STATE_PRESSED;

        if (button_state & (1 << 0)) {
            last_key = LV_KEY_UP;
        } else if (button_state & (1 << 1)) {
            last_key = LV_KEY_DOWN;
        } else if (button_state & (1 << 2)) {
            last_key = LV_KEY_LEFT;
        } else if (button_state & (1 << 3)) {
            last_key = LV_KEY_RIGHT;
        } else if (button_state & (1 << 4)) {
            last_key = LV_KEY_ENTER;
        } else if (button_state & (1 << 5)) {
            last_key = LV_KEY_ESC;
        }

        data->key = last_key;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = last_key;
    }
}

/**
 * @brief Initialize LVGL input drivers
 * 
 * @return 0 on success, negative error code on failure
 */
int lvgl_input_init(void)
{
    /* Initialize touch input device */
    lv_indev_drv_init(&indev_drv_touch);
    indev_drv_touch.type = LV_INDEV_TYPE_POINTER;
    indev_drv_touch.read_cb = lvgl_touch_read_cb;
    
    indev_touch = lv_indev_drv_register(&indev_drv_touch);
    if (!indev_touch) {
        LOG_ERR("Failed to register touch input device");
        return -ENOMEM;
    }
    
    LOG_INF("LVGL touch input registered");
    
    /* Initialize button input device */
    lv_indev_drv_init(&indev_drv_buttons);
    indev_drv_buttons.type = LV_INDEV_TYPE_KEYPAD;
    indev_drv_buttons.read_cb = lvgl_button_read_cb;
    
    indev_buttons = lv_indev_drv_register(&indev_drv_buttons);
    if (!indev_buttons) {
        LOG_ERR("Failed to register button input device");
        return -ENOMEM;
    }
    
    LOG_INF("LVGL button input registered");
    
    return 0;
}

/**
 * @brief Update touch state (called by touch driver)
 * 
 * @param x X coordinate
 * @param y Y coordinate
 * @param pressed Touch pressed state
 */
void lvgl_input_update_touch(int16_t x, int16_t y, bool pressed)
{
    touch_state.x = x;
    touch_state.y = y;
    touch_state.pressed = pressed;
}

/**
 * @brief Update button state (called by button driver)
 * 
 * @param buttons Button state bitmask
 */
void lvgl_input_update_buttons(uint32_t buttons)
{
    button_state = buttons;
}

#else /* !CONFIG_LVGL */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lvgl_input, LOG_LEVEL_WRN);

int lvgl_input_init(void)
{
    LOG_WRN("LVGL not enabled in Kconfig");
    return -ENOTSUP;
}

void lvgl_input_update_touch(int16_t x, int16_t y, bool pressed)
{
}

void lvgl_input_update_buttons(uint32_t buttons)
{
}

#endif /* CONFIG_LVGL */
