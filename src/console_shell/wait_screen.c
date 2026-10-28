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
 * Shows the AkiraConsole wordmark, a clock card (time + date) and the battery.
 * The backlight is dimmed and the shell thread blocks between repaints, so
 * the cadence here sets the standby current:
 *   - on battery: repaint on the minute rollover or when the battery reading
 *     changes, nothing else;
 *   - on a charger: the battery animates (power is not a concern);
 *   - plug-in or a critical battery: "peek" — the backlight comes back to
 *     the user's level for a few seconds while the animation plays.
 *
 * Layout (SCR_W x 240):
 *   y= 22        "Akira Console" wordmark, large font, centred
 *   y= 52..126   clock card: HH:MM hero font, date below
 *   y=146..176   battery icon (scale 3) + percentage + state
 *   y=212        "Hold HOME to wake"
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

#define PEEK_MS 3000
#define PEEK_FRAME_MS 100    /* repaint cadence while peeking */
#define CHARGE_FRAME_MS 320  /* matches the battery widget's sweep step */

static uint8_t s_saved_brightness = 255; /* restored on exit */
static int s_last_min = -1;              /* last drawn minute; -1 forces redraw */
static bool s_active;
static int64_t s_peek_until;             /* 0 = not peeking */
static uint32_t s_last_batt_seq;
static int s_last_batt_frame = -1;

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
    wc.mon = mo; /* 0-based */
    wc.day = (int)days + 1;
    return wc;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void draw_centred(int y, const char *s, int font, uint16_t col)
{
    int x = MAX((SCR_W - akira_ui_text_w(s, font)) / 2, 0);

    switch (font) {
    case AKIRA_UI_FONT_LARGE:
        akira_display_text_large(x, y, s, col);
        break;
    case AKIRA_UI_FONT_HUGE:
        akira_display_text_huge(x, y, s, col);
        break;
    default:
        akira_display_text(x, y, s, col);
        break;
    }
}

static uint8_t user_brightness(void)
{
#ifdef CONFIG_AKIRA_SETTINGS
    char sv[16] = "";

    if (!akira_settings_get("akira/display/brightness", sv, sizeof(sv))) {
        return (uint8_t)((atoi(sv) * 255 + 50) / 100);
    }
#endif
    return s_saved_brightness;
}

#ifdef CONFIG_AKIRA_POWER_MANAGER
static bool battery_animating(const akira_battery_snapshot_t *b)
{
    return b->valid && b->state == AKIRA_BATT_CHARGING;
}

static void draw_battery(const akira_battery_snapshot_t *b, int64_t now)
{
    if (!b->valid) {
        return;
    }
    const char *state;
    char pct[8];

    switch (b->state) {
    case AKIRA_BATT_CHARGING:     state = "Charging"; break;
    case AKIRA_BATT_FULL:         state = "Charged"; break;
    case AKIRA_BATT_NOT_CHARGING: state = "Not charging"; break;
    default:                      state = b->pct <= CONFIG_AKIRA_BATTERY_LOW_THRESHOLD
                                              ? "Battery low" : "On battery"; break;
    }
    snprintf(pct, sizeof(pct), "%u%%", b->pct);

    int tw = MAX(akira_ui_text_w(pct, AKIRA_UI_FONT_LARGE),
                 akira_ui_text_w(state, AKIRA_UI_FONT_SMALL));
    int total = AKIRA_UI_BATT_W(3) + 14 + tw;
    int x = (SCR_W - total) / 2, y = 146;
    bool green = (b->state == AKIRA_BATT_CHARGING || b->state == AKIRA_BATT_FULL);

    akira_ui_battery(x, y, 3, b->pct, (akira_ui_batt_t)b->state, now);
    akira_display_text_large(x + AKIRA_UI_BATT_W(3) + 14, y - 1, pct,
                             green ? SHELL_C_CHARGE : C_WHITE);
    akira_display_text(x + AKIRA_UI_BATT_W(3) + 14, y + 20, state, C_GRAY);
}
#endif

static void draw_frame(void)
{
    struct wall_clock wc = wait_now();
    int64_t now = k_uptime_get();

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
        snprintf(date_str, sizeof(date_str), "Set the clock");
    }
    s_last_min = wc.valid ? wc.min : -1;

    akira_display_clear(C_BLACK);

    /* Wordmark */
    draw_centred(22, "Akira Console", AKIRA_UI_FONT_LARGE, C_WHITE);

    /* Clock in an elevated dither-shadow card — the focal point. */
    int cw = SCR_W - 80, cardh = 74;
    int cardx = (SCR_W - cw) / 2, cardy = 52;
    akira_ui_dither_card(cardx, cardy, cw, cardh, 14, /*selected=*/false, /*shadow=*/5);
    draw_centred(cardy + 12, time_str, AKIRA_UI_FONT_HUGE, C_WHITE);
    draw_centred(cardy + 50, date_str, AKIRA_UI_FONT_SMALL, C_GRAY);

#ifdef CONFIG_AKIRA_POWER_MANAGER
    {
        akira_battery_snapshot_t b;

        akira_pm_battery_snapshot(&b);
        draw_battery(&b, now);
        s_last_batt_seq = b.seq;
        s_last_batt_frame = akira_ui_battery_frame(b.pct, (akira_ui_batt_t)b.state, now);
    }
#endif

    /* Wake hint */
    draw_centred(212, "Hold HOME to wake", AKIRA_UI_FONT_SMALL, C_GRAY);

    akira_display_flush();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void wait_screen_enter(void)
{
    /* Read and save current brightness so we can restore it on exit. */
    s_saved_brightness = 255;
    s_saved_brightness = user_brightness(); /* NVS value, else full */

    /* Dim to WAIT_BRIGHT_PCT % */
    uint8_t dim_hw = (uint8_t)((WAIT_BRIGHT_PCT * 255 + 50) / 100);
    akira_display_hal_set_brightness(dim_hw);

#ifdef CONFIG_AKIRA_POWER_MANAGER
    akira_pm_notify_blank(true);
#endif

    s_active = true;
    s_peek_until = 0;
    s_last_min = -1; /* force a fresh draw */
    draw_frame();
    LOG_INF("Wait screen entered");
}

void wait_screen_update(void)
{
    if (!s_active) {
        return;
    }
    int64_t now = k_uptime_get();
    bool redraw = false;

    /* End of a peek: back to the dim standby level. */
    if (s_peek_until && now >= s_peek_until) {
        s_peek_until = 0;
        akira_display_hal_set_brightness((uint8_t)((WAIT_BRIGHT_PCT * 255 + 50) / 100));
        redraw = true;
    }

    struct wall_clock wc = wait_now();
    if ((wc.valid ? wc.min : -1) != s_last_min) {
        redraw = true;
    }

#ifdef CONFIG_AKIRA_POWER_MANAGER
    akira_battery_snapshot_t b;

    akira_pm_battery_snapshot(&b);
    if (b.seq != s_last_batt_seq) {
        redraw = true;
    } else if (battery_animating(&b) &&
               akira_ui_battery_frame(b.pct, (akira_ui_batt_t)b.state, now) !=
                   s_last_batt_frame) {
        redraw = true;
    }
#endif

    if (redraw) {
        draw_frame();
    }
}

void wait_screen_peek(void)
{
    if (!s_active) {
        return;
    }
    s_peek_until = k_uptime_get() + PEEK_MS;
    akira_display_hal_set_brightness(user_brightness());
    draw_frame();
}

uint32_t wait_screen_ms_to_next_update(void)
{
    int64_t now = k_uptime_get();

    if (s_peek_until) {
        return PEEK_FRAME_MS;
    }
#ifdef CONFIG_AKIRA_POWER_MANAGER
    {
        akira_battery_snapshot_t b;

        akira_pm_battery_snapshot(&b);
        if (battery_animating(&b)) {
            /* On a charger: step the animation; power is not a concern. */
            return (uint32_t)(CHARGE_FRAME_MS - (now % CHARGE_FRAME_MS)) + 5u;
        }
    }
#endif

    if (!akira_time_is_set())
    {
        /* No wall clock — draw_frame() shows a placeholder that never changes,
         * so there is nothing to refresh on a schedule. */
        return WAIT_SCREEN_UPDATE_MAX_MS;
    }

    int64_t epoch = akira_time_get_epoch() + akira_time_get_tz_offset_s();
    int64_t sec_into_min = ((epoch % 60) + 60) % 60;

    /* +50 ms so we land just past the rollover rather than just before it and
     * burn a second wake.  Epoch resolution is 1 s, so a slightly early wake is
     * harmless anyway: update() sees the same minute, skips the repaint, and
     * the caller recomputes a short timeout. */
    return (uint32_t)((60 - sec_into_min) * 1000) + 50u;
}

void wait_screen_exit(void)
{
    s_active = false;
    s_peek_until = 0;

#ifdef CONFIG_AKIRA_POWER_MANAGER
    akira_pm_notify_blank(false);
#endif

    /* Restore brightness — prefer NVS value, fall back to saved hw byte */
    akira_display_hal_set_brightness(user_brightness());

    LOG_INF("Wait screen exited");
}

void wait_screen_prepare_deep_sleep(void)
{
    /* Paint the panel blank before sleeping — a bistable (Sharp) panel would
     * otherwise hold the last clock frame. Wakes on HOME hold. */
    akira_display_clear(C_BLACK);
    akira_display_flush();

    /* Blank backlit displays before cutting power.
     * Sharp LS0XX is a bistable reflective display — it holds the image
     * without power and does not support display_blanking_on(). Skip it. */
#if defined(CONFIG_DISPLAY) && !defined(CONFIG_LS0XX)
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (device_is_ready(disp))
    {
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
