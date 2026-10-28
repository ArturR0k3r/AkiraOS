/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_power_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_power_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file power_screen.c
 * @brief Settings → Power & battery, and the full-screen Battery page.
 *
 * Auto power-off writes akira/power/sleep_s (0 = never), which the shell main
 * loop re-reads when the settings generation changes.  The display timeout
 * lives in Settings → Display; an older version of this page also wrote an
 * akira/power/dispoff_s key that nothing ever read.
 */

#include <zephyr/kernel.h>
#include <settings/settings.h>
#include <stdlib.h>
#include <stdio.h>

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include "power_screen.h"
#include "shell_theme.h"
#include "status_model.h"
#include "ui/akira_ui.h"
#include "ui/overlay.h"
#include <drivers/power/power_manager.h>

/* ---- Power page --------------------------------------------------------- */

static const char *const g_sleep_labels[] = {
    "Never", "5 min", "15 min", "30 min", "1 hour", "2 hours",
};
static const int g_sleep_values[] = { 0, 300, 900, 1800, 3600, 7200 };
static int g_sleep_idx;

static const char *batt_state_text(const shell_status_t *st)
{
    switch ((akira_ui_batt_t)st->batt_state) {
    case AKIRA_UI_BATT_CHARGING:     return "Charging";
    case AKIRA_UI_BATT_FULL:         return "Charged";
    case AKIRA_UI_BATT_NOT_CHARGING: return "Not charging";
    case AKIRA_UI_BATT_DISCHARGING:  return "On battery";
    default:                         return "Unknown";
    }
}

static void power_enter(void)
{
    char sv[16] = "";
    int secs;

    if (akira_settings_get("akira/power/sleep_s", sv, sizeof(sv)) == 0) {
        secs = atoi(sv);
    } else {
#if defined(CONFIG_AKIRA_POWER_DEEP_SLEEP)
        secs = CONFIG_AKIRA_DEEP_SLEEP_IDLE_S;
#else
        secs = 0;
#endif
    }
    /* Nearest choice, so a value set from the shell/web app still shows. */
    g_sleep_idx = 0;
    for (int i = 1; i < (int)ARRAY_SIZE(g_sleep_values); i++) {
        if (secs > 0 && abs(g_sleep_values[i] - secs) <
                            abs(g_sleep_values[g_sleep_idx] - secs)) {
            g_sleep_idx = i;
        }
    }
}

static int get_sleep(const list_item_t *it)
{
    ARG_UNUSED(it);
    return g_sleep_idx;
}

static void set_sleep(const list_item_t *it, int v)
{
    char sv[16];

    ARG_UNUSED(it);
    g_sleep_idx = v;
    snprintf(sv, sizeof(sv), "%d", g_sleep_values[v]);
    akira_settings_set("akira/power/sleep_s", sv, 0);
}

static bool sleep_supported(const list_item_t *it)
{
    ARG_UNUSED(it);
    return IS_ENABLED(CONFIG_AKIRA_POWER_DEEP_SLEEP);
}

static void val_battery(const list_item_t *it, char *buf, size_t len)
{
    const shell_status_t *st = status_model_get();

    ARG_UNUSED(it);
    if (!st->batt_valid) {
        snprintf(buf, len, "--");
    } else if (st->batt_state == AKIRA_UI_BATT_CHARGING) {
        snprintf(buf, len, "%u%% charging", st->batt_pct);
    } else {
        snprintf(buf, len, "%u%%", st->batt_pct);
    }
}

static void val_source(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "%s", status_model_get()->batt_online ? "USB power" : "Battery");
}

static void power_off_done(bool confirmed, void *ctx)
{
    ARG_UNUSED(ctx);
    if (!confirmed) {
        return;
    }
    /* Does not return on success; the battery is cut ~10 s later. */
    if (akira_pm_ship_mode() != 0) {
        overlay_toast(TOAST_ICON_ERROR, "Couldn't power off", "Charger not found", 2500);
    }
}

static void act_power_off(const list_item_t *it)
{
    ARG_UNUSED(it);
    /* No power button on this board: only USB brings it back. */
    akira_ui_confirm_open("POWER_OFF", "Off until USB is plugged in?", NULL,
                          power_off_done, NULL);
}

static const shell_screen_t battery_screen;

static const list_item_t g_items[] = {
    { .kind = LI_NAV, .label = "Battery", .screen = &battery_screen,
      .value = val_battery },
    { .kind = LI_INFO, .label = "Power source", .value = val_source },
    { .kind = LI_CHOICE, .label = "Deep sleep after", .get = get_sleep,
      .set = set_sleep, .choices = g_sleep_labels,
      .n_choices = ARRAY_SIZE(g_sleep_labels), .enabled = sleep_supported },
    { .kind = LI_ACTION, .label = "Turn off completely", .action = act_power_off },
};

const list_model_t power_page = {
    .title = "Power & battery",
    .items = g_items,
    .count = ARRAY_SIZE(g_items),
    .on_enter = power_enter,
    .live = true,
};

/* ---- Battery page ------------------------------------------------------- */

#define BIG_SCALE 5
#define STAT_Y 150
#define STAT_H 46

static int g_batt_frame;

static void stat_card(int x, int w, const char *label, const char *value)
{
    akira_ui_dither_card(x, STAT_Y, w, STAT_H, 8, false, 2);
    akira_display_text(x + (w - akira_ui_text_w(label, AKIRA_UI_FONT_SMALL)) / 2,
                       STAT_Y + 8, label, SHELL_C_GRAY);
    akira_display_text(x + (w - akira_ui_text_w(value, AKIRA_UI_FONT_SMALL)) / 2,
                       STAT_Y + 26, value, AKIRA_UI_PAPER);
}

static void battery_draw(void)
{
    const shell_status_t *st = status_model_get();
    akira_ui_batt_t state = (akira_ui_batt_t)st->batt_state;
    int64_t now = k_uptime_get();
    char buf[32];

    if (!st->batt_valid) {
        const char *l1 = "No battery detected";
        const char *l2 = st->batt_online ? "Running on USB power" : "Check connector J9";

        akira_display_text((SCR_W - akira_ui_text_w(l1, AKIRA_UI_FONT_SMALL)) / 2, 100,
                           l1, AKIRA_UI_PAPER);
        akira_display_text((SCR_W - akira_ui_text_w(l2, AKIRA_UI_FONT_SMALL)) / 2, 118,
                           l2, SHELL_C_GRAY);
        return;
    }

    /* Hero: the animated battery with the percentage beside it. */
    int bx = 28, by = 44;

    akira_ui_battery(bx, by, BIG_SCALE, st->batt_pct, state, now);

    snprintf(buf, sizeof(buf), "%u%%", st->batt_pct);
    int tx = bx + AKIRA_UI_BATT_W(BIG_SCALE) + 22;

    akira_display_text_huge(tx, by + 2, buf,
                            (state == AKIRA_UI_BATT_CHARGING || state == AKIRA_UI_BATT_FULL)
                                ? SHELL_C_CHARGE : AKIRA_UI_PAPER);
    akira_display_text(tx, by + 36, batt_state_text(st), SHELL_C_GRAY);

    /* Hint for the slow-charge case: the BQ25601's input limit is set by
     * PSEL, so a weak port can leave little for the cell. */
    if (state == AKIRA_UI_BATT_CHARGING && st->batt_ma > 0 && st->batt_ma < 150) {
        akira_display_text(bx, by + 72, "Slow charge: weak USB port?",
                           SHELL_C_WARN);
    } else if (state == AKIRA_UI_BATT_NOT_CHARGING) {
        akira_display_text(bx, by + 72, "Charger paused (temp/fault)",
                           SHELL_C_WARN);
    }

    /* Three stat cards. */
    int gap = 8, w = (SCR_W - 2 * 16 - 2 * gap) / 3;

    snprintf(buf, sizeof(buf), "%d.%02d V", (int)(st->batt_mv / 1000),
             (int)(abs(st->batt_mv) % 1000) / 10);
    stat_card(16, w, "Voltage", buf);
    snprintf(buf, sizeof(buf), "%+d mA", (int)st->batt_ma);
    stat_card(16 + w + gap, w, "Current", buf);
    snprintf(buf, sizeof(buf), "%s", st->batt_online ? "USB" : "Battery");
    stat_card(16 + 2 * (w + gap), w, "Source", buf);
}

static bool battery_tick(void)
{
    const shell_status_t *st = status_model_get();
    int f = akira_ui_battery_frame(st->batt_pct, (akira_ui_batt_t)st->batt_state,
                                   k_uptime_get());

    if (f != g_batt_frame) {
        g_batt_frame = f;
        return true;
    }
    return false;
}

static void battery_key(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_B)) {
        screen_pop();
    }
}

static void battery_status(void)
{
    screen_invalidate(); /* live voltage / current */
}

static const shell_screen_t battery_screen = {
    .name = "battery",
    .title = "Battery",
    .flags = SCR_F_STATUSBAR | SCR_F_FOOTER,
    .hint_r = "B:Back",
    .on_key = battery_key,
    .tick = battery_tick,
    .draw = battery_draw,
    .on_status = battery_status,
};

void battery_screen_open(void)
{
    screen_push(&battery_screen, NULL);
}
