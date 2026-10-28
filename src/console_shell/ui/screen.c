/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file screen.c
 * @brief OS shell screen stack and frame scheduler (see screen.h).
 *
 * Paint order for a full frame:
 *   clear → [screen below a modal + its chrome → scrim] → top screen content
 *   → footer → overlays → status bar
 * The status bar goes last so toasts slide out from underneath it, and so a
 * status-only repaint (clock tick, charging sweep) can redraw just that band
 * over the previous frame without disturbing anything else.
 */

#include "screen.h"
#include "akira_ui.h"
#include "anim.h"
#include "overlay.h"
#include "../shell_theme.h"
#include "../status_model.h"

#include <zephyr/kernel.h>
#include <string.h>

#include <api/akira_display_api.h>
#include <lib/mem_helper.h>

#define SCREEN_STACK_MAX 8

typedef struct {
    const shell_screen_t *s;
    const char *title;
    const char *hint_l;
    const char *hint_r;
} screen_entry_t;

static struct {
    screen_entry_t stack[SCREEN_STACK_MAX];
    int top;              /* index of the top entry; -1 before screen_init() */
    bool full_dirty;
    bool status_dirty;
    bool suspended;
    bool animating;       /* the top screen asked for a redraw last frame */
    int self;             /* stack index whose callback is running */
    int status_frame;     /* battery animation frame last painted */
} g_scr AKIRA_BULK_BSS;

/* ---- stack -------------------------------------------------------------- */

static inline screen_entry_t *top_entry(void)
{
    return (g_scr.top >= 0) ? &g_scr.stack[g_scr.top] : NULL;
}

void screen_init(const shell_screen_t *root)
{
    memset(&g_scr, 0, sizeof(g_scr));
    g_scr.top = -1;
    g_scr.status_frame = -1;
    screen_push(root, NULL);
}

void screen_push(const shell_screen_t *s, void *arg)
{
    if (!s) {
        return;
    }
    if (g_scr.top + 1 >= SCREEN_STACK_MAX) {
        LOG_ERR("Screen stack full — cannot open '%s'", s->name);
        return;
    }
    g_scr.top++;
    g_scr.stack[g_scr.top] = (screen_entry_t){
        .s = s, .title = s->title, .hint_l = s->hint_l, .hint_r = s->hint_r,
    };
    LOG_DBG("push %s (depth %d)", s->name, g_scr.top);
    if (s->enter) {
        g_scr.self = g_scr.top;
        s->enter(arg);
    }
    g_scr.full_dirty = true;
}

/* Pop without resuming the screen underneath. */
static void pop_one(void)
{
    if (g_scr.top <= 0) {
        return;
    }
    const shell_screen_t *s = g_scr.stack[g_scr.top].s;

    LOG_DBG("pop %s", s->name);
    /* Decrement first: leave() may open or close screens of its own. */
    g_scr.self = g_scr.top;
    g_scr.top--;
    if (s->leave) {
        s->leave();
    }
}

static void resume_top(void)
{
    screen_entry_t *e = top_entry();

    if (e && e->s->resume) {
        g_scr.self = g_scr.top;
        e->s->resume();
    }
    g_scr.full_dirty = true;
}

void screen_pop(void)
{
    if (g_scr.top <= 0) {
        return;
    }
    pop_one();
    resume_top();
}

void screen_replace(const shell_screen_t *s, void *arg)
{
    if (g_scr.top <= 0) {
        screen_push(s, arg);
        return;
    }
    pop_one();
    screen_push(s, arg);
}

void screen_pop_to_root(void)
{
    bool popped = false;

    while (g_scr.top > 0 && !(g_scr.stack[g_scr.top].s->flags & SCR_F_STICKY)) {
        pop_one();
        popped = true;
    }
    if (popped) {
        resume_top();
    }
    g_scr.full_dirty = true;
}

void screen_close(const shell_screen_t *s)
{
    for (int i = g_scr.top; i > 0; i--) {
        if (g_scr.stack[i].s == s) {
            while (g_scr.top >= i) {
                pop_one();
            }
            resume_top();
            return;
        }
    }
}

bool screen_is_root(void)
{
    return g_scr.top <= 0;
}

bool screen_is_open(const shell_screen_t *s)
{
    for (int i = 0; i <= g_scr.top; i++) {
        if (g_scr.stack[i].s == s) {
            return true;
        }
    }
    return false;
}

const shell_screen_t *screen_top(void)
{
    screen_entry_t *e = top_entry();

    return e ? e->s : NULL;
}

int screen_depth(void)
{
    return g_scr.top;
}

int screen_self(void)
{
    return g_scr.self;
}

void screen_set_title(const char *title)
{
    screen_entry_t *e = top_entry();

    if (e) {
        e->title = title;
        g_scr.status_dirty = true;
    }
}

void screen_set_hints(const char *hint_l, const char *hint_r)
{
    screen_entry_t *e = top_entry();

    if (e) {
        e->hint_l = hint_l;
        e->hint_r = hint_r;
        g_scr.full_dirty = true;
    }
}

void screen_invalidate(void)
{
    g_scr.full_dirty = true;
}

void screen_invalidate_status(void)
{
    g_scr.status_dirty = true;
}

/* ---- painting ----------------------------------------------------------- */

static void fill_status(akira_ui_status_t *ui, const char *title)
{
    const shell_status_t *st = status_model_get();

    memset(ui, 0, sizeof(*ui));
    ui->title = title;
    ui->clock = st->clock;
    ui->battery_pct = st->batt_valid ? st->batt_pct : -1;
    ui->battery_state = (akira_ui_batt_t)st->batt_state;
    ui->show_wifi = IS_ENABLED(CONFIG_WIFI);
    ui->wifi_on = st->wifi_on;
    ui->show_bt = IS_ENABLED(CONFIG_BT);
    ui->bt_on = st->bt_on;
}

void screen_draw_status_bar(const char *title)
{
    akira_ui_status_t ui;

    fill_status(&ui, title);
    akira_ui_status_bar(&ui);
}

void screen_draw_scrim(void)
{
    /* Every other row in the lightest gray: reads as "dimmed" on the white
     * theme for 120 hlines — a per-pixel dither would cost ~40k writes. */
    for (int y = 0; y < SCR_H; y += 2) {
        akira_display_hline(0, y, SCR_W, SHELL_C_DKGRAY);
    }
}

static const screen_entry_t *status_owner(void)
{
    /* The status bar belongs to the topmost non-modal screen. */
    for (int i = g_scr.top; i >= 0; i--) {
        if (!(g_scr.stack[i].s->flags & SCR_F_MODAL)) {
            return (g_scr.stack[i].s->flags & SCR_F_STATUSBAR) ? &g_scr.stack[i] : NULL;
        }
    }
    return NULL;
}

static void paint_full(void)
{
    int first = g_scr.top;

    /* A modal paints over the screen beneath it. */
    while (first > 0 && (g_scr.stack[first].s->flags & SCR_F_MODAL)) {
        first--;
    }

    akira_display_clear(C_BLACK);
    for (int i = first; i <= g_scr.top; i++) {
        const screen_entry_t *e = &g_scr.stack[i];

        if (i > first) {
            if (i == first + 1) {
                /* Chrome of the dimmed screen goes under the scrim too. */
                const screen_entry_t *base = &g_scr.stack[first];

                if (base->s->flags & SCR_F_FOOTER) {
                    akira_ui_footer(base->hint_l, base->hint_r);
                }
                if (base->s->flags & SCR_F_STATUSBAR) {
                    screen_draw_status_bar(base->title);
                }
            }
            screen_draw_scrim();
        }
        if (e->s->draw) {
            g_scr.self = i;
            e->s->draw();
        }
    }

    const screen_entry_t *top = &g_scr.stack[g_scr.top];

    if (!(top->s->flags & SCR_F_MODAL) && (top->s->flags & SCR_F_FOOTER)) {
        akira_ui_footer(top->hint_l, top->hint_r);
    }
    overlay_draw();
    if (first == g_scr.top) {
        const screen_entry_t *owner = status_owner();

        if (owner) {
            screen_draw_status_bar(owner->title);
        }
    }
    akira_display_flush();
}

static void paint_status_only(void)
{
    const screen_entry_t *owner = status_owner();

    /* Under a modal the bar sits beneath the scrim — leave it be. */
    if (!owner || owner != top_entry()) {
        return;
    }
    screen_draw_status_bar(owner->title);
    /* Only the bar's band changed: ~17 KB over SPI instead of the 154 KB
     * frame — this is the charging sweep and the minute tick. */
    akira_display_flush_rows(0, SBAR_H + 2);
}

/* ---- frame loop ----------------------------------------------------------- */

void screen_dispatch_key(uint32_t just)
{
    if (!just || g_scr.suspended || g_scr.top < 0) {
        return;
    }
    if (overlay_handle_key(just)) {
        return;
    }
    screen_entry_t *e = top_entry();

    if (e->s->on_key) {
        g_scr.self = g_scr.top;
        e->s->on_key(just);
    }
}

void screen_frame(void)
{
    if (g_scr.suspended || g_scr.top < 0) {
        return;
    }

    /* tick() may push or pop, so look the top up again afterwards. */
    const shell_screen_t *s = top_entry()->s;
    g_scr.self = g_scr.top;
    bool redraw = s->tick ? s->tick() : false;

    g_scr.animating = redraw;
    if (overlay_tick()) {
        redraw = true;
    }

    if (status_model_update_battery()) {
        g_scr.status_dirty = true;
    }
    const shell_status_t *st = status_model_get();
    int frame = st->batt_valid
                    ? akira_ui_battery_frame(st->batt_pct, (akira_ui_batt_t)st->batt_state,
                                             k_uptime_get())
                    : 0;

    if (frame != g_scr.status_frame) {
        g_scr.status_frame = frame;
        g_scr.status_dirty = true;
    }

    if (redraw || g_scr.full_dirty) {
        g_scr.full_dirty = false;
        g_scr.status_dirty = false;
        paint_full();
    } else if (g_scr.status_dirty) {
        g_scr.status_dirty = false;
        paint_status_only();
    }
}

void screen_status_tick(void)
{
    if (status_model_update()) {
        g_scr.status_dirty = true;
    }
    if (g_scr.suspended || g_scr.top < 0) {
        return;
    }
    const shell_screen_t *s = top_entry()->s;

    if (s->on_status) {
        g_scr.self = g_scr.top;
        s->on_status();
    }
}

void screen_set_suspended(bool suspended)
{
    g_scr.suspended = suspended;
    if (!suspended) {
        status_model_update();
        g_scr.full_dirty = true;
    }
}

bool screen_is_suspended(void)
{
    return g_scr.suspended;
}

bool screen_is_animating(void)
{
    return !g_scr.suspended && (g_scr.animating || overlay_is_animating());
}

void screen_run_busy(const char *label, void (*fn)(void *), void *arg)
{
    if (!g_scr.suspended) {
        int w = 220, h = 64;
        int x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;

        screen_draw_scrim();
        akira_ui_dither_card(x, y, w, h, 12, false, 4);
        akira_ui_spinner(x + 30, y + h / 2, 12, k_uptime_get());
        akira_display_text(x + 54, y + (h - 10) / 2, label ? label : "Working",
                           AKIRA_UI_PAPER);
        akira_display_flush();
    }
    fn(arg);
    g_scr.full_dirty = true;
}
