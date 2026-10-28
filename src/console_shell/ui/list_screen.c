/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file list_screen.c
 * @brief Declarative settings list (see list_screen.h).
 */

#include "list_screen.h"
#include "akira_ui.h"
#include "anim.h"
#include "../shell_theme.h"

#include <zephyr/kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <lib/mem_helper.h>

#define ROW_H        34
#define ROW_TOP      (SCREEN_CONTENT_Y + 2)
#define ROW_X        12
#define ROW_W        (SCR_W - 30)  /* leaves a lane for the scrollbar */
#define CARD_H       (ROW_H - 6)
#define VISIBLE_ROWS ((SCREEN_CONTENT_B - ROW_TOP) / ROW_H)

#define SEL_MS       130
#define RISE_PX      18
#define RISE_MS      240
#define RISE_STAGGER 35

#define MAX_DEPTH    8

typedef struct {
    const list_model_t *m;
    int sel;
    int scroll;        /* first visible row */
    anim_t hl;         /* highlight position, px from the list's origin */
    anim_t scroll_px;  /* scroll position, px */
    int64_t t_enter;
} list_state_t;

static list_state_t g_ls[MAX_DEPTH] AKIRA_BULK_BSS;

static inline list_state_t *self(void)
{
    return &g_ls[CLAMP(screen_self(), 0, MAX_DEPTH - 1)];
}

static bool item_enabled(const list_item_t *it)
{
    return !it->enabled || it->enabled(it);
}

static bool item_selectable(const list_item_t *it)
{
    return it->kind != LI_INFO || it->value; /* info rows still scroll into view */
}

/* Rows can disappear under us (e.g. an app uninstalled from the Apps page). */
static void clamp_sel(list_state_t *ls)
{
    int last = MAX(ls->m->count - 1, 0);

    if (ls->sel > last) {
        ls->sel = last;
        anim_set(&ls->hl, last * ROW_H);
    }
    int max_scroll = MAX(ls->m->count - VISIBLE_ROWS, 0);

    if (ls->scroll > max_scroll) {
        ls->scroll = max_scroll;
        anim_set(&ls->scroll_px, max_scroll * ROW_H);
    }
}

/* ---- value text ------------------------------------------------------- */

static void item_value(const list_item_t *it, char *buf, size_t len)
{
    buf[0] = '\0';
    if (it->value) {
        it->value(it, buf, len);
        return;
    }
    switch (it->kind) {
    case LI_CHOICE: {
        int v = it->get ? it->get(it) : 0;

        if (it->choices && v >= 0 && v < it->n_choices) {
            snprintf(buf, len, "%s", it->choices[v]);
        }
        break;
    }
    case LI_STEPPER:
        snprintf(buf, len, "%d%s", it->get ? it->get(it) : 0, it->unit ? it->unit : "");
        break;
    default:
        break;
    }
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_toggle(int x, int y, bool on, uint16_t fg, uint16_t bg)
{
    const int tw = 30, th = 16, r = 8;

    if (on) {
        akira_display_rounded_rect_fill(x, y, tw, th, r, fg);
        akira_display_circle_fill(x + tw - 8, y + th / 2, 5, bg);
    } else {
        akira_display_rounded_rect(x, y, tw, th, r, fg);
        akira_display_circle_fill(x + 8, y + th / 2, 5, fg);
    }
}

static void draw_row(const list_state_t *ls, int i, int y, bool inverted)
{
    const list_item_t *it = &ls->m->items[i];
    bool en = item_enabled(it);
    uint16_t fg = inverted ? AKIRA_UI_INK : (en ? AKIRA_UI_PAPER : SHELL_C_GRAY);
    uint16_t bg = inverted ? AKIRA_UI_PAPER : AKIRA_UI_INK;
    int ty = y + (CARD_H - 10) / 2;
    int rx = ROW_X + ROW_W - 12;
    char val[32];

    akira_display_text(ROW_X + 12, ty, it->label, fg);

    switch (it->kind) {
    case LI_NAV:
        /* chevron */
        akira_display_line(rx - 4, ty + 1, rx, ty + 5, fg);
        akira_display_line(rx, ty + 5, rx - 4, ty + 9, fg);
        akira_display_line(rx - 5, ty + 1, rx - 1, ty + 5, fg);
        akira_display_line(rx - 1, ty + 5, rx - 5, ty + 9, fg);
        rx -= 14;
        break;
    case LI_TOGGLE:
        draw_toggle(rx - 30, y + (CARD_H - 16) / 2, it->get && it->get(it), fg, bg);
        rx -= 40;
        break;
    default:
        break;
    }

    item_value(it, val, sizeof(val));
    if (!val[0]) {
        return;
    }
    bool adjustable = inverted && en &&
                      (it->kind == LI_CHOICE || it->kind == LI_STEPPER);
    int vw = akira_ui_text_w(val, AKIRA_UI_FONT_SMALL);

    if (adjustable) {
        /* < value > while focused: the row is adjustable with LEFT/RIGHT */
        akira_display_text(rx - 8, ty, ">", fg);
        akira_display_text(rx - 16 - vw, ty, val, fg);
        akira_display_text(rx - 28 - vw, ty, "<", fg);
    } else {
        akira_display_text(rx - vw, ty, val,
                           (it->kind == LI_INFO && !inverted) ? SHELL_C_GRAY : fg);
    }
}

/* Vertical offset of row i while the page is still rising in. */
static int rise_offset(const list_state_t *ls, int visible_index, int64_t now)
{
    int64_t t = now - ls->t_enter - visible_index * RISE_STAGGER;

    if (t >= RISE_MS) {
        return 0;
    }
    return RISE_PX - RISE_PX * anim_ease(ANIM_EASE_OUT, t, RISE_MS) / 256;
}

static void list_draw(void)
{
    list_state_t *ls = self();
    const list_model_t *m = ls->m;
    int64_t now = k_uptime_get();
    int scroll = anim_value(&ls->scroll_px);
    int hl = anim_value(&ls->hl);

    if (!m) {
        return;
    }
    clamp_sel(ls);
    if (m->count == 0) {
        return;
    }

    /* Idle cards first, then the highlight over them, then all labels. */
    int first = MAX(scroll / ROW_H - 1, 0);
    int last = MIN(first + VISIBLE_ROWS + 2, m->count);

    for (int i = first; i < last; i++) {
        int y = ROW_TOP + i * ROW_H - scroll + rise_offset(ls, i - ls->scroll, now);

        if (y + CARD_H < SCREEN_CONTENT_Y - 4 || y > SCREEN_CONTENT_B) {
            continue;
        }
        if (m->items[i].kind == LI_INFO) {
            /* info rows: flat, just a dotted rule under them */
            for (int x = ROW_X + 8; x < ROW_X + ROW_W - 8; x += 3) {
                akira_display_pixel(x, y + CARD_H, SHELL_C_DKGRAY);
            }
        } else {
            akira_ui_dither_card(ROW_X, y, ROW_W, CARD_H, 8, false, 2);
        }
    }

    /* No highlight on a page with nothing to pick (e.g. "No apps installed"). */
    bool has_sel = item_selectable(&m->items[ls->sel]);

    if (has_sel) {
        int hy = ROW_TOP + hl - scroll + rise_offset(ls, ls->sel - ls->scroll, now);

        akira_ui_dither_card(ROW_X, hy, ROW_W, CARD_H, 8, true, 3);
    }

    for (int i = first; i < last; i++) {
        int y = ROW_TOP + i * ROW_H - scroll + rise_offset(ls, i - ls->scroll, now);

        if (y + CARD_H < SCREEN_CONTENT_Y - 4 || y > SCREEN_CONTENT_B) {
            continue;
        }
        draw_row(ls, i, y, has_sel && abs(hl - i * ROW_H) < ROW_H / 2);
    }

    /* Scrollbar lane */
    if (m->count > VISIBLE_ROWS) {
        int track_y = ROW_TOP, track_h = VISIBLE_ROWS * ROW_H - 6;
        int max_scroll = (m->count - VISIBLE_ROWS) * ROW_H;
        int thumb_h = MAX(track_h * VISIBLE_ROWS / m->count, 12);
        int thumb_y = track_y + (track_h - thumb_h) * CLAMP(scroll, 0, max_scroll) / max_scroll;

        akira_display_vline(SCR_W - 9, track_y, track_h, SHELL_C_DKGRAY);
        akira_display_rounded_rect_fill(SCR_W - 11, thumb_y, 5, thumb_h, 2, AKIRA_UI_PAPER);
    }

    if (m->draw_extra) {
        m->draw_extra(ROW_TOP + m->count * ROW_H - scroll);
    }
}

/* ---- behaviour ---------------------------------------------------------- */

static void move_sel(list_state_t *ls, int to)
{
    const list_model_t *m = ls->m;

    to = CLAMP(to, 0, m->count - 1);
    if (to == ls->sel) {
        return;
    }
    ls->sel = to;
    anim_to(&ls->hl, to * ROW_H, SEL_MS, ANIM_EASE_OUT);

    int scroll = ls->scroll;

    if (to < scroll) {
        scroll = to;
    } else if (to >= scroll + VISIBLE_ROWS) {
        scroll = to - VISIBLE_ROWS + 1;
    }
    if (scroll != ls->scroll) {
        ls->scroll = scroll;
        anim_to(&ls->scroll_px, scroll * ROW_H, SEL_MS + 40, ANIM_EASE_OUT);
    }
    screen_invalidate();
}

static void adjust(const list_item_t *it, int dir, bool from_a)
{
    if (!item_enabled(it) || !it->set) {
        return;
    }
    int v = it->get ? it->get(it) : 0;

    switch (it->kind) {
    case LI_TOGGLE:
        it->set(it, from_a ? !v : (dir > 0));
        break;
    case LI_CHOICE:
        if (it->n_choices > 0) {
            it->set(it, (v + dir + it->n_choices) % it->n_choices);
        }
        break;
    case LI_STEPPER:
        if (!from_a) {
            it->set(it, CLAMP(v + dir * MAX(it->step, 1), it->min, it->max));
        }
        break;
    default:
        return;
    }
    screen_invalidate();
}

static void list_key(uint32_t just)
{
    list_state_t *ls = self();
    const list_model_t *m = ls->m;

    if (!m) {
        return;
    }
    if (just & BIT(AKIRA_BTN_B)) {
        screen_pop();
        return;
    }
    clamp_sel(ls);
    if (m->count == 0) {
        return;
    }
    if (just & BIT(AKIRA_BTN_UP)) {
        int to = ls->sel - 1;

        while (to > 0 && !item_selectable(&m->items[to])) {
            to--;
        }
        move_sel(ls, to);
    }
    if (just & BIT(AKIRA_BTN_DOWN)) {
        int to = ls->sel + 1;

        while (to < m->count - 1 && !item_selectable(&m->items[to])) {
            to++;
        }
        move_sel(ls, to);
    }

    const list_item_t *it = &m->items[ls->sel];

    if (just & BIT(AKIRA_BTN_LEFT)) {
        adjust(it, -1, false);
    }
    if (just & BIT(AKIRA_BTN_RIGHT)) {
        adjust(it, +1, false);
    }
    if (just & BIT(AKIRA_BTN_A)) {
        if (!item_enabled(it)) {
            return;
        }
        switch (it->kind) {
        case LI_NAV:
            screen_push(it->screen, it->arg);
            return;
        case LI_ACTION:
            if (it->action) {
                it->action(it);
            }
            screen_invalidate();
            return;
        default:
            adjust(it, +1, true);
            break;
        }
    }
}

static bool list_tick(void)
{
    list_state_t *ls = self();
    int64_t settle = ls->t_enter + RISE_MS + VISIBLE_ROWS * RISE_STAGGER;

    return anim_running(&ls->hl) || anim_running(&ls->scroll_px) ||
           k_uptime_get() < settle;
}

static void list_enter(void *arg)
{
    list_state_t *ls = self();
    const list_model_t *m = arg;

    memset(ls, 0, sizeof(*ls));
    ls->m = m;
    ls->t_enter = k_uptime_get();
    anim_set(&ls->hl, 0);
    anim_set(&ls->scroll_px, 0);
    if (m) {
        screen_set_title(m->title);
        if (m->hint_l || m->hint_r) {
            screen_set_hints(m->hint_l ? m->hint_l : "A:Select",
                             m->hint_r ? m->hint_r : "B:Back");
        }
        if (m->on_enter) {
            m->on_enter();
        }
        while (ls->sel < m->count - 1 && !item_selectable(&m->items[ls->sel])) {
            ls->sel++;
        }
        anim_set(&ls->hl, ls->sel * ROW_H);
    }
}

static void list_leave(void)
{
    list_state_t *ls = self();

    if (ls->m && ls->m->on_leave) {
        ls->m->on_leave();
    }
    ls->m = NULL;
}

static void list_resume(void)
{
    list_state_t *ls = self();

    /* A child page may have changed what our rows show. */
    if (ls->m && ls->m->on_enter) {
        ls->m->on_enter();
    }
}

static void list_status(void)
{
    list_state_t *ls = self();

    if (ls->m && ls->m->live) {
        screen_invalidate();
    }
}

const shell_screen_t list_screen = {
    .name = "list",
    .flags = SCR_F_STATUSBAR | SCR_F_FOOTER,
    .hint_l = "A:Select",
    .hint_r = "B:Back",
    .enter = list_enter,
    .leave = list_leave,
    .resume = list_resume,
    .on_key = list_key,
    .tick = list_tick,
    .draw = list_draw,
    .on_status = list_status,
};

void list_screen_open(const list_model_t *model)
{
    screen_push(&list_screen, (void *)model);
}
