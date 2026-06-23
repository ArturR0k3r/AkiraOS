/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_wait_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_wait_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file wait_screen.c
 * @brief Idle power-save screen for AkiraConsole.
 *
 * Fully static — drawn once on enter, never redrawn.
 * No CPU wakeups for clock ticks; battery is read once at entry.
 *
 * Layout (SCR_W × 240):
 *   y= 30   full-width hairline
 *   y= 96   "AKIRA"  large font, centred, white
 *   y=120   full-width hairline
 *   y=152   battery %  small, centred, dark-gray  (if available)
 *   y=200   full-width hairline
 *   y=213   "Hold HOME to wake"  small, centred, dark-gray
 */

#include "wait_screen.h"
#include "shell_theme.h"

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

#include <api/akira_display_api.h>
#include <drivers/platform_hal.h>

#if defined(CONFIG_DISPLAY)
#include <zephyr/drivers/display.h>
#endif
#if defined(CONFIG_AKIRA_POWER_DEEP_SLEEP)
#include <zephyr/drivers/gpio.h>
#endif

#ifdef CONFIG_AKIRA_SETTINGS
#include <settings/settings.h>
#endif

#ifdef CONFIG_AKIRA_POWER_MANAGER
#include <drivers/power/power_manager.h>
#endif

/* Brightness level used while wait screen is active (30% of full range). */
#define WAIT_BRIGHT_PCT 30

static uint8_t s_saved_brightness = 255; /* restored on exit */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void draw_centred_small(int y, const char *s, uint16_t col)
{
    int tw = (int)strlen(s) * 8;
    int x = (SCR_W - tw) / 2;
    if (x < 0)
        x = 0;
    akira_display_text(x, y, s, col);
}

static void draw_centred_large(int y, const char *s, uint16_t col)
{
    int tw = (int)strlen(s) * 11; /* akira_display_text_large uses 11px per char */
    int x = (SCR_W - tw) / 2;
    if (x < 0)
        x = 0;
    akira_display_text_large(x, y, s, col);
}

/* Drawn once on enter — never updated, no periodic CPU wakeup needed. */
static void draw_frame(void)
{
    char batt_str[8] = "";

#ifdef CONFIG_AKIRA_POWER_MANAGER
    {
        uint8_t pct = 0;
        if (akira_pm_get_battery_level(&pct) == 0)
        {
            snprintf(batt_str, sizeof(batt_str), "%u%%", (unsigned)pct);
        }
    }
#endif

    akira_display_clear(C_BLACK);

    /* Top hairline */
    akira_display_hline(0, 30, SCR_W, C_WHITE);

    /* Wordmark */
    draw_centred_large(96, "AKIRA", C_WHITE);

    /* Bottom-of-wordmark hairline */
    akira_display_hline(0, 120, SCR_W, C_WHITE);

    /* Battery level (static snapshot taken at enter time) */
    if (batt_str[0])
    {
        draw_centred_small(152, batt_str, C_DKGRAY);
    }

    /* Lower hairline */
    akira_display_hline(0, 200, SCR_W, C_WHITE);

    /* Wake hint */
    draw_centred_small(213, "Hold HOME to wake", C_DKGRAY);

    akira_display_flush();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void wait_screen_enter(void)
{
    /* Read and save current brightness so we can restore it on exit. */
    s_saved_brightness = 255;
#ifdef CONFIG_AKIRA_SETTINGS
    {
        char sv[16] = "";
        if (!akira_settings_get("akira/display/brightness", sv, sizeof(sv)))
        {
            int pct = atoi(sv);
            s_saved_brightness = (uint8_t)((pct * 255 + 50) / 100);
        }
    }
#endif

    /* Dim to WAIT_BRIGHT_PCT % */
    uint8_t dim_hw = (uint8_t)((WAIT_BRIGHT_PCT * 255 + 50) / 100);
    akira_display_hal_set_brightness(dim_hw);

#ifdef CONFIG_AKIRA_POWER_MANAGER
    akira_pm_enable_low_power_mode(true);
#endif

    draw_frame();
    LOG_INF("Wait screen entered");
}

void wait_screen_update(void)
{
    /* Screen is static — nothing to update. */
}

void wait_screen_exit(void)
{
#ifdef CONFIG_AKIRA_POWER_MANAGER
    akira_pm_enable_low_power_mode(false);
#endif

    /* Restore brightness — prefer NVS value, fall back to saved hw byte */
#ifdef CONFIG_AKIRA_SETTINGS
    {
        char sv[16] = "";
        if (!akira_settings_get("akira/display/brightness", sv, sizeof(sv)))
        {
            int pct = atoi(sv);
            akira_display_hal_set_brightness((uint8_t)((pct * 255 + 50) / 100));
        }
        else
        {
            akira_display_hal_set_brightness(s_saved_brightness);
        }
    }
#else
    akira_display_hal_set_brightness(s_saved_brightness);
#endif

    LOG_INF("Wait screen exited");
}

void wait_screen_prepare_deep_sleep(void)
{
#if defined(CONFIG_DISPLAY)
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (device_is_ready(disp)) {
        display_blanking_on(disp);
    }
#endif

#ifdef CONFIG_AKIRA_POWER_DEEP_SLEEP
    /* Configure HOME button (GPIO0, active-low) as level wakeup source.
     * Zephyr's ESP32 GPIO driver calls rtc_gpio_wakeup_enable() for level
     * triggers on RTC-capable GPIOs, enabling wakeup from deep sleep. */
    static const struct gpio_dt_spec home_gpio =
        GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
    if (device_is_ready(home_gpio.port)) {
        gpio_pin_interrupt_configure_dt(&home_gpio, GPIO_INT_LEVEL_ACTIVE);
    }
    akira_pm_set_mode(POWER_MODE_DEEP_SLEEP);
#endif
    LOG_INF("Deep sleep entered");
}
