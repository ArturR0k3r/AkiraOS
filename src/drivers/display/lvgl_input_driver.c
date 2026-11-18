/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lvgl_input_driver.c
 * @brief LVGL v9 input driver — maps AkiraConsole buttons / touch to LVGL.
 *
 * Button bitmask source, in order of preference:
 *   1. CONFIG_AKIRA_INPUT_API — the native input API's akira_input_get_bitmask()
 *      (AkiraConsole branch: bit N set while the button with zephyr,code N is
 *      held).
 *   2. CONFIG_AKIRA_LVGL_INPUT_ZEPHYR — an INPUT_CALLBACK_DEFINE handler that
 *      bridges the Zephyr input subsystem (gpio-keys D-pad / face buttons,
 *      touch controllers, native_sim input, ...) into the same bitmask layout.
 *   3. Otherwise, whatever a bespoke driver pushes via
 *      lvgl_input_update_buttons().
 * Touch comes from INPUT_ABS_X/Y + INPUT_BTN_TOUCH under
 * CONFIG_AKIRA_LVGL_INPUT_ZEPHYR, or from lvgl_input_update_touch().
 *
 * Bit positions are the AKIRA_BTN_* codes (api/akira_input_api.h), which
 * match the gpio-keys zephyr,code values in the AkiraConsole DTS.
 *
 * X is the dedicated HOME button.  A long-press (>
 * CONFIG_AKIRA_HOME_BUTTON_GPIO_LONG_MS) on X raises the home callback
 * registered via lvgl_input_set_home_callback().
 *
 * Key mapping to LVGL:
 *   UP/DOWN/LEFT/RIGHT → LV_KEY_UP / DOWN / LEFT / RIGHT
 *   A                  → LV_KEY_ENTER  (confirm / select)
 *   B                  → LV_KEY_ESC    (back / cancel)
 *   X (short press)    → LV_KEY_HOME
 *   X (long press)     → fires home callback once per press
 *   Y                  → LV_KEY_END    (context menu)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "lvgl_input_driver.h"

#if defined(CONFIG_LVGL)
#include <lvgl.h>
#include <zephyr/sys/atomic.h>
#include <api/akira_input_api.h>
#if defined(CONFIG_AKIRA_LVGL_INPUT_ZEPHYR)
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#endif

LOG_MODULE_REGISTER(lvgl_input, LOG_LEVEL_INF);

#if defined(CONFIG_AKIRA_HOME_BUTTON_GPIO_LONG_MS)
#define HOME_LONG_PRESS_MS  CONFIG_AKIRA_HOME_BUTTON_GPIO_LONG_MS
#else
#define HOME_LONG_PRESS_MS  500
#endif

static lv_indev_t *indev_touch;
static lv_indev_t *indev_keypad;

/* Touch state — filled by the Zephyr input handler or lvgl_input_update_touch() */
static struct {
    int16_t x;
    int16_t y;
    bool    pressed;
} g_touch;

/* Button bitmask (bit N = AKIRA_BTN_* code N) when the native input API is not
 * the source — fed by the Zephyr input handler or lvgl_input_update_buttons(). */
static atomic_t g_buttons;

/* HOME long-press tracking */
static int64_t g_x_press_start_ms;
static bool    g_x_held;
static bool    g_home_fired;

/* Optional home callback */
static void (*g_home_cb)(void *user_data);
static void  *g_home_cb_data;

void lvgl_input_set_home_callback(void (*cb)(void *), void *user_data)
{
    g_home_cb      = cb;
    g_home_cb_data = user_data;
}

#if defined(CONFIG_AKIRA_LVGL_INPUT_ZEPHYR)
static inline void set_button_bit(uint8_t bit, bool pressed)
{
    if (pressed) {
        atomic_or(&g_buttons, (atomic_val_t)BIT(bit));
    } else {
        atomic_and(&g_buttons, ~(atomic_val_t)BIT(bit));
    }
}

/**
 * @brief Zephyr input subsystem event handler
 *
 * Bridges the project's real input devices into the LVGL indev state.
 * Registered for every device (dev == NULL) so the same LVGL front-end works
 * across boards without hardcoding a controller. Button codes accept both the
 * AkiraConsole gpio-keys mapping (AKIRA_BTN_*, zephyr,code 2..9) and the
 * standard Linux key / D-pad / gamepad codes for portability.
 */
static void lvgl_input_zephyr_cb(struct input_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);

    switch (evt->type) {
    case INPUT_EV_ABS:
        /* Absolute pointer axes from a touch / pen controller. */
        if (evt->code == INPUT_ABS_X) {
            g_touch.x = (int16_t)evt->value;
        } else if (evt->code == INPUT_ABS_Y) {
            g_touch.y = (int16_t)evt->value;
        }
        break;

    case INPUT_EV_KEY: {
        const bool pressed = (evt->value != 0);

        switch (evt->code) {
        case INPUT_BTN_TOUCH:
            g_touch.pressed = pressed;
            break;

        case AKIRA_BTN_UP:
        case INPUT_KEY_UP:
        case INPUT_BTN_DPAD_UP:
            set_button_bit(AKIRA_BTN_UP, pressed);
            break;
        case AKIRA_BTN_DOWN:
        case INPUT_KEY_DOWN:
        case INPUT_BTN_DPAD_DOWN:
            set_button_bit(AKIRA_BTN_DOWN, pressed);
            break;
        case AKIRA_BTN_LEFT:
        case INPUT_KEY_LEFT:
        case INPUT_BTN_DPAD_LEFT:
            set_button_bit(AKIRA_BTN_LEFT, pressed);
            break;
        case AKIRA_BTN_RIGHT:
        case INPUT_KEY_RIGHT:
        case INPUT_BTN_DPAD_RIGHT:
            set_button_bit(AKIRA_BTN_RIGHT, pressed);
            break;
        case AKIRA_BTN_A:      /* A / South / Enter -> LV_KEY_ENTER (select) */
        case INPUT_KEY_ENTER:
        case INPUT_BTN_SOUTH:
            set_button_bit(AKIRA_BTN_A, pressed);
            break;
        case AKIRA_BTN_B:      /* B / East / Esc -> LV_KEY_ESC (back) */
        case INPUT_KEY_ESC:
        case INPUT_BTN_EAST:
            set_button_bit(AKIRA_BTN_B, pressed);
            break;
        case AKIRA_BTN_X:      /* X -> LV_KEY_HOME / HOME long-press */
            set_button_bit(AKIRA_BTN_X, pressed);
            break;
        case AKIRA_BTN_Y:      /* Y -> LV_KEY_END (context menu) */
            set_button_bit(AKIRA_BTN_Y, pressed);
            break;
        default:
            /* Unmapped code — ignore. */
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

static uint32_t read_buttons(void)
{
#if defined(CONFIG_AKIRA_INPUT_API)
    /* Live bitmask from the native input API. Bit N is set when the button
     * with zephyr,code==N is held; AKIRA_BTN_* match the DTS codes. */
    return akira_input_get_bitmask();
#else
    return (uint32_t)atomic_get(&g_buttons);
#endif
}

/* ------------------------------------------------------------------ */
/* Read callbacks (LVGL v9 signature)                                  */
/* ------------------------------------------------------------------ */

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ARG_UNUSED(indev);
    data->state   = g_touch.pressed ? LV_INDEV_STATE_PRESSED
                                    : LV_INDEV_STATE_RELEASED;
    data->point.x = g_touch.x;
    data->point.y = g_touch.y;
}

static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ARG_UNUSED(indev);

    uint32_t buttons = read_buttons();

    /* HOME long-press detection */
    if (buttons & BIT(AKIRA_BTN_X)) {
        int64_t now = k_uptime_get();

        if (!g_x_held) {
            g_x_held           = true;
            g_x_press_start_ms = now;
            g_home_fired       = false;
        } else if (!g_home_fired &&
                   (now - g_x_press_start_ms) >= HOME_LONG_PRESS_MS) {
            g_home_fired = true;
            LOG_INF("HOME long-press detected");
            if (g_home_cb) {
                g_home_cb(g_home_cb_data);
            }
            /* Suppress the key event for this press */
            buttons &= ~BIT(AKIRA_BTN_X);
        }
    } else {
        g_x_held = false;
    }

    if (buttons & BIT(AKIRA_BTN_UP))    { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_UP;    return; }
    if (buttons & BIT(AKIRA_BTN_DOWN))  { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_DOWN;  return; }
    if (buttons & BIT(AKIRA_BTN_LEFT))  { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_LEFT;  return; }
    if (buttons & BIT(AKIRA_BTN_RIGHT)) { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_RIGHT; return; }
    if (buttons & BIT(AKIRA_BTN_A))     { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_ENTER; return; }
    if (buttons & BIT(AKIRA_BTN_B))     { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_ESC;   return; }
    if (buttons & BIT(AKIRA_BTN_X))     { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_HOME;  return; }
    if (buttons & BIT(AKIRA_BTN_Y))     { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_END;   return; }

    data->state = LV_INDEV_STATE_RELEASED;
    data->key   = 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int lvgl_input_init(void)
{
    /* Touch pointer device */
    indev_touch = lv_indev_create();
    if (!indev_touch) {
        LOG_ERR("Failed to create touch indev");
        return -ENOMEM;
    }
    lv_indev_set_type(indev_touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_touch, touch_read_cb);

    /* Keypad device (D-pad + buttons) */
    indev_keypad = lv_indev_create();
    if (!indev_keypad) {
        LOG_ERR("Failed to create keypad indev");
        return -ENOMEM;
    }
    lv_indev_set_type(indev_keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev_keypad, keypad_read_cb);

    LOG_INF("LVGL input driver initialised (HOME=X, long-press=%dms)",
            HOME_LONG_PRESS_MS);
    return 0;
}

lv_indev_t *lvgl_input_get_keypad(void)
{
    return indev_keypad;
}

void lvgl_input_update_touch(int16_t x, int16_t y, bool pressed)
{
    g_touch.x       = x;
    g_touch.y       = y;
    g_touch.pressed = pressed;
}

void lvgl_input_update_buttons(uint32_t buttons)
{
    /* Bit N = AKIRA_BTN_* code N. Ignored under CONFIG_AKIRA_INPUT_API, where
     * the bitmask is sourced from akira_input_get_bitmask() directly. */
    atomic_set(&g_buttons, (atomic_val_t)buttons);
}

#else /* !CONFIG_LVGL */

LOG_MODULE_REGISTER(lvgl_input, LOG_LEVEL_WRN);

int lvgl_input_init(void)
{
    LOG_WRN("LVGL not enabled in Kconfig");
    return -ENOTSUP;
}

/* lvgl_input_get_keypad() is a static inline NULL stub in the header. */

void lvgl_input_update_touch(int16_t x, int16_t y, bool pressed)
{
    ARG_UNUSED(x); ARG_UNUSED(y); ARG_UNUSED(pressed);
}

void lvgl_input_update_buttons(uint32_t buttons)
{
    ARG_UNUSED(buttons);
}

void lvgl_input_set_home_callback(void (*cb)(void *), void *user_data)
{
    ARG_UNUSED(cb); ARG_UNUSED(user_data);
}

#endif /* CONFIG_LVGL */
