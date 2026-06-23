/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_wait_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_wait_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file wait_screen.c
 * @brief Idle wait / power-save screensaver.
 *
 * Layout (320x240):
 *   y= 10   "POWER SAVE"  small, centred, dark-gray
 *   y=100   HH:MM:SS      large font, centred, white
 *   y=130   battery %     small, centred, dark-gray
 *   y=220   "Press any button to wake"  small, centred, dark-gray
 */

#include "wait_screen.h"
#include "shell_theme.h"

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

#include <api/akira_display_api.h>
#include <lib/akira_time.h>
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

static void build_time_str(char *buf, size_t len)
{
    int64_t epoch = akira_time_get_epoch();
    if (akira_time_is_set())
    {
        int64_t local = epoch + (int64_t)akira_time_get_tz_offset_s();
        int64_t day_sec = local % 86400;
        if (day_sec < 0)
            day_sec += 86400;
        snprintf(buf, len, "%02u:%02u:%02u",
                 (unsigned)(day_sec / 3600),
                 (unsigned)((day_sec % 3600) / 60),
                 (unsigned)(day_sec % 60));
    }
    else
    {
        uint32_t s = (uint32_t)epoch;
        snprintf(buf, len, "%02u:%02u:%02u",
                 (s / 3600U) % 24U, (s % 3600U) / 60U, s % 60U);
    }
}

/* Full redraw — used only on enter so static labels are drawn once. */
static void draw_frame(void)
{
    char time_str[10];
    char batt_str[8] = "";

    build_time_str(time_str, sizeof(time_str));

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

    draw_centred_small(10, "POWER SAVE", C_DKGRAY);
    draw_centred_large(96, time_str, C_WHITE);

    if (batt_str[0])
    {
        draw_centred_small(130, batt_str, C_DKGRAY);
    }

    draw_centred_small(220, "Hold HOME to wake", C_DKGRAY);

    akira_display_flush();
}

/* Incremental update — only repaint time and battery to avoid full-screen blink. */
static void draw_dynamic(void)
{
    char time_str[10];
    char batt_str[8] = "";

    build_time_str(time_str, sizeof(time_str));

#ifdef CONFIG_AKIRA_POWER_MANAGER
    {
        uint8_t pct = 0;
        if (akira_pm_get_battery_level(&pct) == 0)
        {
            snprintf(batt_str, sizeof(batt_str), "%u%%", (unsigned)pct);
        }
    }
#endif

    /* Erase time row (FONT_11X18 = 18px tall, centred at y=96). */
    akira_display_rect(0, 96, SCR_W, 18, C_BLACK);
    draw_centred_large(96, time_str, C_WHITE);

    /* Erase battery row (FONT_7X10 = 10px tall, centred at y=130). */
    akira_display_rect(0, 130, SCR_W, 10, C_BLACK);
    if (batt_str[0])
    {
        draw_centred_small(130, batt_str, C_DKGRAY);
    }

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
    draw_dynamic();
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
