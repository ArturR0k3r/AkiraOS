/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file overlay.c
 * @brief Toasts, install-progress card and charging splash (see overlay.h).
 */

#include "overlay.h"
#include "anim.h"
#include "screen.h"
#include "../shell_theme.h"
#include "../status_model.h"

#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <lib/mem_helper.h>

/* ---- geometry ----------------------------------------------------------- */
#define TOAST_X      16
#define TOAST_H      40
#define TOAST_Y      (SBAR_H + 6)
#define TOAST_IN_MS  280
#define TOAST_OUT_MS 200
#define TOAST_QUEUE  4

#define PROG_H       58
#define PROG_Y       (FOOT_Y - PROG_H - 8)
#define PROG_HOLD_MS 1600
#define PROG_STALE_MS 20000 /* no updates for this long → just hide */

#define SPLASH_W     220
#define SPLASH_H     144
#define SPLASH_MS    2600

typedef enum { T_IDLE, T_IN, T_HOLD, T_OUT } toast_phase_t;

typedef struct {
    int handle;
    uint8_t icon;
    bool busy;
    int duration_ms;
    char l1[36];
    char l2[40];
} toast_t;

static struct {
    /* toasts: q[0] is the one on screen */
    toast_t q[TOAST_QUEUE];
    int n;
    int next_handle;
    toast_phase_t phase;
    anim_t y;
    int64_t hold_until;
    int frame;

    /* install progress card */
    bool prog_on;
    bool prog_hiding;
    bool prog_done;
    bool prog_ok;
    bool prog_dirty;
    int prog_pct;
    char prog_name[24];
    char prog_msg[48];
    anim_t prog_y;
    int64_t prog_last_ms;
    int64_t prog_hide_at;

    /* charging splash */
    bool splash_on;
    int64_t splash_until;
    anim_t splash_y;
    int splash_frame;
} g_ov AKIRA_BULK_BSS;

/* ---- toasts ------------------------------------------------------------- */

static void toast_pop_front(void)
{
    if (g_ov.n > 0) {
        memmove(&g_ov.q[0], &g_ov.q[1], sizeof(toast_t) * (g_ov.n - 1));
        g_ov.n--;
    }
    g_ov.phase = T_IDLE;
}

int overlay_toast(toast_icon_t icon, const char *line1, const char *line2,
                  int duration_ms)
{
    if (icon == TOAST_ICON_BATTERY) {
        /* Battery news supersedes older battery news: drop queued ones and
         * hurry the one on screen out, so "12% remaining" never shows up
         * after the cell has already reached 4%. */
        for (int i = g_ov.n - 1; i >= 1; i--) {
            if (g_ov.q[i].icon == TOAST_ICON_BATTERY) {
                memmove(&g_ov.q[i], &g_ov.q[i + 1], sizeof(toast_t) * (g_ov.n - i - 1));
                g_ov.n--;
            }
        }
        if (g_ov.n > 0 && g_ov.q[0].icon == TOAST_ICON_BATTERY) {
            overlay_toast_dismiss(g_ov.q[0].handle);
        }
    }
    if (g_ov.n == TOAST_QUEUE) {
        /* Full: drop the oldest queued (never the one on screen). */
        memmove(&g_ov.q[1], &g_ov.q[2], sizeof(toast_t) * (TOAST_QUEUE - 2));
        g_ov.n--;
    }
    toast_t *t = &g_ov.q[g_ov.n++];

    memset(t, 0, sizeof(*t));
    t->handle = ++g_ov.next_handle;
    t->icon = (uint8_t)icon;
    t->duration_ms = duration_ms;
    snprintf(t->l1, sizeof(t->l1), "%s", line1 ? line1 : "");
    snprintf(t->l2, sizeof(t->l2), "%s", line2 ? line2 : "");
    return t->handle;
}

void overlay_toast_dismiss(int handle)
{
    for (int i = 0; i < g_ov.n; i++) {
        if (g_ov.q[i].handle != handle) {
            continue;
        }
        if (i == 0 && g_ov.phase != T_IDLE) {
            /* On screen: let it slide out rather than vanish. */
            g_ov.q[0].duration_ms = 1;
            if (g_ov.phase == T_HOLD) {
                g_ov.hold_until = k_uptime_get();
            }
        } else {
            memmove(&g_ov.q[i], &g_ov.q[i + 1], sizeof(toast_t) * (g_ov.n - i - 1));
            g_ov.n--;
        }
        return;
    }
}

void overlay_toast_set_busy(int handle, bool busy)
{
    for (int i = 0; i < g_ov.n; i++) {
        if (g_ov.q[i].handle == handle) {
            g_ov.q[i].busy = busy;
        }
    }
}

static int toast_frame(const toast_t *t, int64_t now)
{
    if (t->busy) {
        return 1 + (int)((now / 350) % 4);
    }
    if (t->icon == TOAST_ICON_BATTERY) {
        const shell_status_t *st = status_model_get();

        return akira_ui_battery_frame(st->batt_pct, (akira_ui_batt_t)st->batt_state, now);
    }
    return 0;
}

static bool toast_tick(int64_t now)
{
    bool redraw = false;

    if (g_ov.phase == T_IDLE && g_ov.n > 0) {
        g_ov.phase = T_IN;
        anim_start(&g_ov.y, -TOAST_H - 6, TOAST_Y, TOAST_IN_MS, ANIM_EASE_OUT_BACK);
    }
    if (g_ov.phase == T_IN) {
        redraw = true;
        if (!anim_running(&g_ov.y)) {
            g_ov.phase = T_HOLD;
            g_ov.hold_until = g_ov.q[0].duration_ms == TOAST_STICKY
                                  ? INT64_MAX : now + g_ov.q[0].duration_ms;
        }
    }
    if (g_ov.phase == T_HOLD) {
        if (now >= g_ov.hold_until) {
            g_ov.phase = T_OUT;
            anim_start(&g_ov.y, TOAST_Y, -TOAST_H - 6, TOAST_OUT_MS, ANIM_EASE_INOUT);
        } else {
            int f = toast_frame(&g_ov.q[0], now);

            if (f != g_ov.frame) {
                g_ov.frame = f;
                redraw = true;
            }
        }
    }
    if (g_ov.phase == T_OUT) {
        redraw = true;
        if (!anim_running(&g_ov.y)) {
            toast_pop_front();
        }
    }
    return redraw;
}

static void toast_icon(const toast_t *t, int x, int y, int64_t now)
{
    switch (t->icon) {
    case TOAST_ICON_BATTERY: {
        const shell_status_t *st = status_model_get();

        akira_ui_battery(x, y + (TOAST_H - AKIRA_UI_BATT_H(2)) / 2, 2,
                         st->batt_pct, (akira_ui_batt_t)st->batt_state, now);
        break;
    }
    case TOAST_ICON_OK:
        akira_display_circle_fill(x + 12, y + TOAST_H / 2, 11, SHELL_C_CHARGE);
        for (int i = 0; i < 2; i++) {
            akira_display_line(x + 6, y + 20 + i, x + 10, y + 24 + i, AKIRA_UI_INK);
            akira_display_line(x + 10, y + 24 + i, x + 18, y + 15 + i, AKIRA_UI_INK);
        }
        break;
    case TOAST_ICON_ERROR:
        akira_display_circle_fill(x + 12, y + TOAST_H / 2, 11, SHELL_C_CRIT);
        for (int i = 0; i < 2; i++) {
            akira_display_line(x + 7 + i, y + 15, x + 16 + i, y + 24, AKIRA_UI_INK);
            akira_display_line(x + 16 + i, y + 15, x + 7 + i, y + 24, AKIRA_UI_INK);
        }
        break;
    case TOAST_ICON_SD: {
        int cx = x + 4, cy = y + 9;

        akira_display_rect(cx, cy + 4, 18, 18, AKIRA_UI_PAPER);
        akira_display_rect(cx + 4, cy, 14, 4, AKIRA_UI_PAPER);
        for (int i = 0; i < 4; i++) {
            akira_display_rect(cx + 6 + i * 3, cy + 1, 2, 5, AKIRA_UI_INK);
        }
        break;
    }
    case TOAST_ICON_INFO:
        akira_display_circle(x + 12, y + TOAST_H / 2, 11, AKIRA_UI_PAPER);
        akira_display_rect(x + 11, y + 13, 3, 3, AKIRA_UI_PAPER);
        akira_display_rect(x + 11, y + 18, 3, 9, AKIRA_UI_PAPER);
        break;
    default:
        break;
    }
}

static void toast_draw(int64_t now)
{
    if (g_ov.phase == T_IDLE || g_ov.n == 0) {
        return;
    }
    const toast_t *t = &g_ov.q[0];
    int y = anim_value(&g_ov.y);
    int w = SCR_W - 2 * TOAST_X;
    int tx = TOAST_X + 12;

    akira_ui_dither_card(TOAST_X, y, w, TOAST_H, 10, false, 3);
    if (t->icon != TOAST_ICON_NONE) {
        toast_icon(t, TOAST_X + 10, y, now);
        tx = TOAST_X + 10 + (t->icon == TOAST_ICON_BATTERY ? AKIRA_UI_BATT_W(2) : 24) + 12;
    }

    bool two = t->l2[0] || t->busy;
    akira_display_text(tx, y + (two ? 8 : 15), t->l1, AKIRA_UI_PAPER);
    if (two) {
        char l2[48];

        snprintf(l2, sizeof(l2), "%s%.*s", t->l2,
                 t->busy ? (int)((now / 350) % 4) : 0, "...");
        akira_display_text(tx, y + 22, l2, SHELL_C_GRAY);
    }
}

/* ---- install progress card ------------------------------------------ */

void overlay_progress(const char *name, int pct, const char *msg)
{
    int64_t now = k_uptime_get();

    if (!g_ov.prog_on || g_ov.prog_hiding) {
        g_ov.prog_on = true;
        g_ov.prog_hiding = false;
        anim_start(&g_ov.prog_y, SCR_H, PROG_Y, 240, ANIM_EASE_OUT);
    }
    g_ov.prog_done = false;
    g_ov.prog_hide_at = 0;
    g_ov.prog_pct = CLAMP(pct, 0, 100);
    if (name && name[0]) {
        snprintf(g_ov.prog_name, sizeof(g_ov.prog_name), "%s", name);
    }
    if (msg) {
        snprintf(g_ov.prog_msg, sizeof(g_ov.prog_msg), "%s", msg);
    }
    g_ov.prog_last_ms = now;
    g_ov.prog_dirty = true;
}

void overlay_progress_finish(bool ok, const char *msg)
{
    if (!g_ov.prog_on) {
        overlay_progress(NULL, ok ? 100 : 0, msg);
    }
    g_ov.prog_done = true;
    g_ov.prog_ok = ok;
    if (ok) {
        g_ov.prog_pct = 100;
    }
    if (msg) {
        snprintf(g_ov.prog_msg, sizeof(g_ov.prog_msg), "%s", msg);
    }
    g_ov.prog_hide_at = k_uptime_get() + PROG_HOLD_MS;
    g_ov.prog_dirty = true;
}

bool overlay_progress_active(void)
{
    return g_ov.prog_on && !g_ov.prog_done;
}

static bool progress_tick(int64_t now)
{
    if (!g_ov.prog_on) {
        return false;
    }
    bool redraw = g_ov.prog_dirty || anim_running(&g_ov.prog_y);

    g_ov.prog_dirty = false;
    if (!g_ov.prog_done && (now - g_ov.prog_last_ms) > PROG_STALE_MS) {
        g_ov.prog_hide_at = now; /* producer went quiet: don't strand the card */
    }
    if (!g_ov.prog_hiding && g_ov.prog_hide_at && now >= g_ov.prog_hide_at) {
        g_ov.prog_hiding = true;
        anim_to(&g_ov.prog_y, SCR_H, 200, ANIM_EASE_INOUT);
        redraw = true;
    }
    if (g_ov.prog_hiding && !anim_running(&g_ov.prog_y)) {
        g_ov.prog_on = false;
        g_ov.prog_hiding = false;
        redraw = true;
    }
    return redraw;
}

static void progress_draw(void)
{
    if (!g_ov.prog_on) {
        return;
    }
    int y = anim_value(&g_ov.prog_y);
    int x = 16, w = SCR_W - 32;
    char pct[8];

    akira_ui_dither_card(x, y, w, PROG_H, 10, false, 3);
    akira_display_text(x + 12, y + 9, g_ov.prog_name[0] ? g_ov.prog_name : "Installing",
                       AKIRA_UI_PAPER);
    snprintf(pct, sizeof(pct), "%d%%", g_ov.prog_pct);
    akira_display_text(x + w - 12 - akira_ui_text_w(pct, AKIRA_UI_FONT_SMALL), y + 9,
                       pct, AKIRA_UI_PAPER);
    akira_display_text(x + 12, y + 23, g_ov.prog_msg,
                       g_ov.prog_done && !g_ov.prog_ok ? SHELL_C_CRIT : SHELL_C_GRAY);
    akira_ui_meter(x + 12, y + 39, w - 24, 10, g_ov.prog_pct, 100);
}

/* ---- charging splash ---------------------------------------------------- */

void overlay_charging_splash(void)
{
    g_ov.splash_on = true;
    g_ov.splash_until = k_uptime_get() + SPLASH_MS;
    anim_start(&g_ov.splash_y, 28, 0, 340, ANIM_EASE_OUT_BACK);
}

static bool splash_tick(int64_t now)
{
    if (!g_ov.splash_on) {
        return false;
    }
    if (now >= g_ov.splash_until) {
        g_ov.splash_on = false;
        return true;
    }
    if (anim_running(&g_ov.splash_y)) {
        return true;
    }
    const shell_status_t *st = status_model_get();
    int f = akira_ui_battery_frame(st->batt_pct, (akira_ui_batt_t)st->batt_state, now);

    if (f != g_ov.splash_frame) {
        g_ov.splash_frame = f;
        return true;
    }
    return false;
}

static void splash_draw(int64_t now)
{
    if (!g_ov.splash_on) {
        return;
    }
    const shell_status_t *st = status_model_get();
    akira_ui_batt_t state = (akira_ui_batt_t)st->batt_state;
    int x = (SCR_W - SPLASH_W) / 2;
    int y = (SCR_H - SPLASH_H) / 2 + 6 + anim_value(&g_ov.splash_y);
    char pct[8], sub[40];

    screen_draw_scrim();
    akira_ui_dither_card(x, y, SPLASH_W, SPLASH_H, 16, false, 5);

    akira_ui_battery((SCR_W - AKIRA_UI_BATT_W(4)) / 2, y + 16, 4, st->batt_pct, state, now);

    snprintf(pct, sizeof(pct), "%u%%", st->batt_pct);
    akira_display_text_huge((SCR_W - akira_ui_text_w(pct, AKIRA_UI_FONT_HUGE)) / 2,
                            y + 66, pct,
                            state == AKIRA_UI_BATT_DISCHARGING ? AKIRA_UI_PAPER : SHELL_C_CHARGE);

    if (state == AKIRA_UI_BATT_FULL) {
        snprintf(sub, sizeof(sub), "Fully charged");
    } else if (state == AKIRA_UI_BATT_CHARGING) {
        if (st->batt_ma > 0) {
            snprintf(sub, sizeof(sub), "Charging  +%d mA", (int)st->batt_ma);
        } else {
            snprintf(sub, sizeof(sub), "Charging");
        }
    } else if (state == AKIRA_UI_BATT_NOT_CHARGING) {
        snprintf(sub, sizeof(sub), "Plugged in, not charging");
    } else {
        snprintf(sub, sizeof(sub), "On battery");
    }
    akira_display_text((SCR_W - akira_ui_text_w(sub, AKIRA_UI_FONT_SMALL)) / 2,
                       y + 108, sub, SHELL_C_GRAY);
}

/* ---- framework hooks ---------------------------------------------------- */

bool overlay_handle_key(uint32_t just)
{
    if (g_ov.splash_on && just) {
        g_ov.splash_on = false;
        screen_invalidate();
        return true;
    }
    return false;
}

bool overlay_tick(void)
{
    int64_t now = k_uptime_get();
    bool redraw = toast_tick(now);

    redraw |= progress_tick(now);
    redraw |= splash_tick(now);
    return redraw;
}

void overlay_draw(void)
{
    int64_t now = k_uptime_get();

    progress_draw();
    splash_draw(now);
    toast_draw(now); /* topmost: it sits under the status bar, over everything */
}

bool overlay_is_animating(void)
{
    return g_ov.phase == T_IN || g_ov.phase == T_OUT ||
           anim_running(&g_ov.prog_y) || anim_running(&g_ov.splash_y);
}

void overlay_clear(void)
{
    g_ov.n = 0;
    g_ov.phase = T_IDLE;
    g_ov.prog_on = false;
    g_ov.prog_hiding = false;
    g_ov.splash_on = false;
}
