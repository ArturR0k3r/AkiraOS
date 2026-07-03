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
 * Shows a clock (time + date) over the AKIRA wordmark. Redrawn only when the
 * minute changes — the shell thread already ticks wait_screen_update() once a
 * second while blanked, so this adds no new CPU wakeups. After deep sleep the
 * bistable Sharp panel holds the last-drawn frame without power.
 *
 * Layout (SCR_W × 240):
 *   y= 26   full-width hairline
 *   y= 42   "AKIRA"  large font, centred, white
 *   y= 76   full-width hairline
 *   y=104   "HH:MM"  large font, centred, white      (time)
 *   y=140   "Sat 28 Jun 2026"  small, centred, gray  (date)
 *   y=178   full-width hairline
 *   y=192   battery %  small, centred, dark-gray      (if available)
 *   y=214   "Hold HOME to wake"  small, centred, dark-gray
 */

#include "wait_screen.h"
#include "shell_theme.h"
#include "ui/akira_ui.h"

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

#include <api/akira_display_api.h>
#include <drivers/platform_hal.h>
#include <lib/akira_time.h>
#include <stdlib.h>

#if defined(CONFIG_DISPLAY)
#include <zephyr/drivers/display.h>
#endif
#if defined(CONFIG_AKIRA_POWER_DEEP_SLEEP)
#if defined(CONFIG_SOC_ESP32S3)
/* CONFIG_POWEROFF=y compiles sleep_gpio/event/console/modem/cpu so that
 * sleep_modes.c (always built) can resolve its own symbol references.
 * sys_poweroff() → z_sys_poweroff() → esp_deep_sleep_start(). */
#include <zephyr/sys/poweroff.h>
#include <esp_sleep.h>
#endif
#endif

#ifdef CONFIG_AKIRA_SETTINGS
#include <settings/settings.h>
#endif

#ifdef CONFIG_AKIRA_POWER_MANAGER
#include <drivers/power/power_manager.h>
#endif

/* Phase 1 backlight level.
 * Sharp LS0XX is a reflective memory display — it holds the image without power
 * and is readable in ambient light, so we cut the backlight completely.
 * Backlit TFT panels need a minimum ~10% to remain visible in dim conditions. */
#if defined(CONFIG_LS0XX)
#define WAIT_BRIGHT_PCT 0
#else
#define WAIT_BRIGHT_PCT 10
#endif

static uint8_t s_saved_brightness = 255; /* restored on exit */
static int s_last_min = -1;              /* last drawn minute; -1 forces redraw */

/* ------------------------------------------------------------------ */
/* Local wall-clock decomposition                                      */
/* ------------------------------------------------------------------ */

struct wall_clock
{
    int year, mon, day, hour, min, wday; /* wday: 0=Sun .. 6=Sat */
    bool valid;
};

static const char *const k_wday[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const k_mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const int k_mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

/* Decompose the current local epoch into calendar fields (Gregorian). */
static struct wall_clock wait_now(void)
{
    struct wall_clock wc = {0};
    wc.valid = akira_time_is_set();

    int64_t epoch = akira_time_get_epoch() + akira_time_get_tz_offset_s();
    int64_t days = epoch / 86400;
    int64_t rem = epoch % 86400;
    if (rem < 0)
    {
        rem += 86400;
        days--;
    }

    wc.hour = (int)(rem / 3600);
    wc.min = (int)((rem % 3600) / 60);
    /* 1970-01-01 was a Thursday (index 4). */
    wc.wday = (int)(((days % 7) + 4 + 7) % 7);

    int y = 1970;
    while (1)
    {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        int ydays = leap ? 366 : 365;
        if (days < ydays)
            break;
        days -= ydays;
        y++;
    }
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    int mo = 0;
    while (mo < 11)
    {
        int md = (mo == 1 && leap) ? 29 : k_mdays[mo];
        if (days < md)
            break;
        days -= md;
        mo++;
    }
    wc.year = y;
    wc.mon = mo;            /* 0-based */
    wc.day = (int)days + 1;
    return wc;
}

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

/* Redrawn on enter and whenever the minute changes (see wait_screen_update). */
static void draw_frame(void)
{
    struct wall_clock wc = wait_now();

    char time_str[8];
    char date_str[20];
    if (wc.valid)
    {
        snprintf(time_str, sizeof(time_str), "%02d:%02d", wc.hour, wc.min);
        snprintf(date_str, sizeof(date_str), "%s %02d %s %04d",
                 k_wday[wc.wday], wc.day, k_mon[wc.mon], wc.year);
    }
    else
    {
        /* Clock never set — show placeholders rather than a 1970 date. */
        snprintf(time_str, sizeof(time_str), "--:--");
        snprintf(date_str, sizeof(date_str), "set clock: date set");
    }
    s_last_min = wc.valid ? wc.min : -1;

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

    /* Wordmark */
    draw_centred_large(28, "AKIRA", C_WHITE);

    /* Clock in an elevated dither-shadow card — the focal point. */
    int cw = SCR_W - 80, cardh = 74;
    int cardx = (SCR_W - cw) / 2, cardy = 70;
    akira_ui_dither_card(cardx, cardy, cw, cardh, 14, /*selected=*/false, /*shadow=*/5);
    draw_centred_large(cardy + 16, time_str, C_WHITE);
    draw_centred_small(cardy + 48, date_str, C_WHITE);

    /* Battery level */
    if (batt_str[0])
    {
        draw_centred_small(178, batt_str, C_WHITE);
    }

    /* Wake hint */
    draw_centred_small(212, "Hold HOME to wake", C_WHITE);

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

    s_last_min = -1; /* force a fresh draw */
    draw_frame();
    LOG_INF("Wait screen entered");
}

void wait_screen_update(void)
{
    /* Called ~1×/s by the shell thread while blanked. Only repaint when the
     * minute rolls over, so the clock stays current without per-second flushes. */
    struct wall_clock wc = wait_now();
    int cur = wc.valid ? wc.min : -1;
    if (cur != s_last_min)
    {
        draw_frame();
    }
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
    /* Blank backlit displays before cutting power.
     * Sharp LS0XX is a bistable reflective display — it holds the image
     * without power and does not support display_blanking_on(). Skip it. */
#if defined(CONFIG_DISPLAY) && !defined(CONFIG_LS0XX)
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (device_is_ready(disp)) {
        display_blanking_on(disp);
    }
#endif

#ifdef CONFIG_AKIRA_POWER_DEEP_SLEEP
#if defined(CONFIG_SOC_ESP32S3)
    /* ESP32-S3: ext0 wakeup on GPIO0 (HOME button, active-low, pull-up).
     * Button pressed = GPIO0 LOW → level = 0.
     * sys_poweroff() → z_sys_poweroff() → esp_deep_sleep_start(). */
    esp_sleep_enable_ext0_wakeup(0 /* GPIO_NUM_0 */, 0 /* level LOW */);
    LOG_INF("Deep sleep — wake on GPIO0 LOW (HOME button)");
    sys_poweroff();
#else
    akira_pm_set_mode(POWER_MODE_DEEP_SLEEP);
    LOG_INF("Deep sleep entered");
#endif
#endif
}
