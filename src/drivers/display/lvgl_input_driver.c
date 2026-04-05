/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file lvgl_input_driver.c
 * @brief LVGL v9 input driver — maps AkiraConsole GPIO buttons to LVGL key events.
 *
 * Button layout (AkiraConsole ESP32-S3):
 *   GPIO4=UP   GPIO5=DOWN  GPIO6=LEFT  GPIO7=RIGHT
 *   GPIO15=A   GPIO16=B    GPIO17=X    GPIO41=Y
 *
 * X (GPIO17) is the dedicated HOME button.  A long-press (>500 ms) on X
 * raises the home callback registered via lvgl_input_set_home_callback().
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

#if defined(CONFIG_LVGL)
#include <lvgl.h>
#include <api/akira_input_api.h>

LOG_MODULE_REGISTER(lvgl_input, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Button GPIO pins (match AkiraConsole hardware)                     */
/* ------------------------------------------------------------------ */
/* GPIO numbers noted for reference; bit positions in the bitmask from
 * akira_input_get_bitmask() are the zephyr,code values from DTS, which
 * match the AKIRA_BTN_* constants defined in akira_native_api.h.        */

/* DTS zephyr,code values — must stay in sync with the overlay */
#define _BTN_UP    2
#define _BTN_DOWN  3
#define _BTN_LEFT  4
#define _BTN_RIGHT 5
#define _BTN_A     6
#define _BTN_B     7
#define _BTN_X     8   /* HOME */
#define _BTN_Y     9

#define HOME_LONG_PRESS_MS  CONFIG_AKIRA_HOME_BUTTON_GPIO_LONG_MS

static lv_indev_t *indev_touch;
static lv_indev_t *indev_keypad;

/* Touch state — filled by lvgl_input_update_touch() */
static struct {
    int16_t x;
    int16_t y;
    bool    pressed;
} g_touch;

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

    /* Read the live button bitmask from the Zephyr input subsystem.
     * Bit N is set when the button with zephyr,code==N is held.
     * AKIRA_BTN_* constants (akira_native_api.h) match the DTS codes. */
    uint32_t buttons = akira_input_get_bitmask();

    /* HOME long-press detection */
    if (buttons & BIT(_BTN_X)) {
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
            buttons &= ~BIT(_BTN_X);
        }
    } else {
        g_x_held = false;
    }

    if (buttons & BIT(_BTN_UP))    { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_UP;    return; }
    if (buttons & BIT(_BTN_DOWN))  { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_DOWN;  return; }
    if (buttons & BIT(_BTN_LEFT))  { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_LEFT;  return; }
    if (buttons & BIT(_BTN_RIGHT)) { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_RIGHT; return; }
    if (buttons & BIT(_BTN_A))     { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_ENTER; return; }
    if (buttons & BIT(_BTN_B))     { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_ESC;   return; }
    if (buttons & BIT(_BTN_X))     { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_HOME;  return; }
    if (buttons & BIT(_BTN_Y))     { data->state = LV_INDEV_STATE_PRESSED; data->key = LV_KEY_END;   return; }

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

    LOG_INF("LVGL input driver initialised (HOME=GPIO17, long-press=%dms)",
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
    /* Bitmask is now sourced from akira_input_get_bitmask() directly.
     * This stub kept for API compatibility; has no effect. */
    ARG_UNUSED(buttons);
}

#else /* !CONFIG_LVGL */

LOG_MODULE_REGISTER(lvgl_input, LOG_LEVEL_WRN);

int lvgl_input_init(void)
{
    return -ENOTSUP;
}

void *lvgl_input_get_keypad(void) { return NULL; }

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
