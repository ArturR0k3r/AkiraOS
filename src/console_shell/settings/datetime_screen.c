/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_datetime_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_datetime_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file datetime_screen.c
 * @brief Settings → Date & time: live clock, UTC offset, manual set, SNTP.
 *
 * A custom screen rather than a list_model_t page because of the live clock
 * panel and the asynchronous SNTP sync.  The sync runs on the system work
 * queue; the shell thread polls for its result in tick() and applies it
 * there.  It must not be applied from the work item: akira_time_set_epoch()
 * persists through akira_settings_set(), which itself queues work on the
 * system work queue and waits for it forever.
 */

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <lib/akira_time.h>
#include "datetime_screen.h"
#include "shell_theme.h"
#include "ui/akira_ui.h"
#include "ui/anim.h"
#include "ui/overlay.h"
#include "ui/screen.h"

#if defined(CONFIG_SNTP)
#include <zephyr/net/sntp.h>
#endif

/* ------------------------------------------------------------------ */
/* Gregorian helpers                                                   */
/* ------------------------------------------------------------------ */

static bool is_leap(int y) { return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0); }

static int days_in_month(int m, int y)
{
    static const int8_t md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12) return 30;
    return (m == 2 && is_leap(y)) ? 29 : md[m - 1];
}

/* Decompose UTC epoch + tz_offset into display fields */
static void epoch_decompose(int64_t epoch, int32_t tz_s,
                             int *y, int *mo, int *d,
                             int *h, int *mi, int *s)
{
    int64_t local = epoch + tz_s;
    int64_t day_sec = local % 86400;
    if (day_sec < 0) day_sec += 86400;
    int64_t days = local / 86400;
    if (local < 0 && day_sec != 0) days--;

    *h  = (int)(day_sec / 3600);
    *mi = (int)((day_sec % 3600) / 60);
    *s  = (int)(day_sec % 60);

    int yr = 1970;
    while (1) {
        int ydays = is_leap(yr) ? 366 : 365;
        if (days < ydays) break;
        days -= ydays; yr++;
    }
    int mn = 1;
    for (; mn < 12; mn++) {
        int mdays = days_in_month(mn, yr);
        if (days < mdays) break;
        days -= mdays;
    }
    *y  = yr;
    *mo = mn;
    *d  = (int)days + 1;
}

/* Convert local broken-down time back to UTC epoch */
static int64_t compose_epoch(int y, int mo, int d,
                              int h, int mi, int s,
                              int32_t tz_s)
{
    /* Days from 1970-01-01 to y-mo-d */
    int64_t days = 0;
    for (int yr = 1970; yr < y; yr++)
        days += is_leap(yr) ? 366 : 365;
    for (int m = 1; m < mo; m++)
        days += days_in_month(m, y);
    days += (d - 1);
    int64_t local_epoch = days * 86400 + h * 3600 + mi * 60 + s;
    return local_epoch - tz_s;
}

static void epoch_to_str(int64_t epoch, int32_t tz_s, char *buf, size_t len)
{
    int y, mo, d, h, mi, s;
    epoch_decompose(epoch, tz_s, &y, &mo, &d, &h, &mi, &s);
    snprintf(buf, len, "%04d-%02d-%02d  %02d:%02d:%02d", y, mo, d, h, mi, s);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/* Clock panel across the top of the content band, field rows below it. */
#define PANEL_X      12
#define PANEL_Y      (SCREEN_CONTENT_Y + 2)
#define PANEL_W      (SCR_W - 30)
#define PANEL_H      30

#define ROW_TOP      (PANEL_Y + PANEL_H + 6)
#define ROW_H        28
#define ROW_X        12
#define ROW_W        (SCR_W - 30)  /* leaves a lane for the scrollbar */
#define CARD_H       (ROW_H - 5)
#define VISIBLE_ROWS ((SCREEN_CONTENT_B - ROW_TOP) / ROW_H)
#define LIST_B       (ROW_TOP + VISIBLE_ROWS * ROW_H)

#define SEL_MS       130
#define SPIN_STEP_MS 90    /* akira_ui_spinner() advances every 90 ms */

/* ------------------------------------------------------------------ */
/* Screen state                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    ITEM_NTP = 0,
    ITEM_TZ,
    ITEM_YEAR,
    ITEM_MONTH,
    ITEM_DAY,
    ITEM_HOUR,
    ITEM_MIN,
    ITEM_SET,
    ITEM_BACK,
    NUM_ITEMS,
} dt_item_t;

static const char *const g_labels[NUM_ITEMS] = {
    "Sync NTP", "UTC Offset",
    "Year", "Month", "Day", "Hour", "Minute",
    "Set Clock", "Back"
};

static struct {
    int sel;
    int scroll;          /* first visible row */
    anim_t hl;           /* highlight position, px from the list's origin */
    anim_t scroll_px;    /* scroll position, px */

    int32_t tz_s;        /* UTC offset being shown / edited */
    int year;
    int month;
    int day;
    int hour;
    int min;

    bool syncing;        /* waiting for the SNTP work item */
    int64_t deadline;    /* give up waiting at this uptime */
    int64_t spin_phase;  /* last spinner step painted */
} g_dt;

static void clamp_day(void)
{
    int mx = days_in_month(g_dt.month, g_dt.year);
    if (g_dt.day > mx) g_dt.day = mx;
    if (g_dt.day < 1)  g_dt.day = 1;
}

/* Load the edit fields from @p epoch in the current UTC offset. */
static void load_fields(int64_t epoch)
{
    int s;

    epoch_decompose(epoch, g_dt.tz_s, &g_dt.year, &g_dt.month, &g_dt.day,
                    &g_dt.hour, &g_dt.min, &s);
}

static bool is_adjustable(int i)
{
    return i >= ITEM_TZ && i <= ITEM_MIN;
}

static void update_hints(void)
{
    if (g_dt.syncing) {
        screen_set_hints(NULL, "B:Cancel");
        return;
    }
    switch (g_dt.sel) {
    case ITEM_NTP:  screen_set_hints("A:Sync", "B:Back"); break;
    case ITEM_SET:  screen_set_hints("A:Set clock", "B:Back"); break;
    case ITEM_BACK: screen_set_hints("A:Back", "B:Back"); break;
    default:        screen_set_hints("</>:Adjust", "B:Back"); break;
    }
}

/* ------------------------------------------------------------------ */
/* SNTP (system work queue → polled by the shell thread)               */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_SNTP)
#define NTP_SERVER        "pool.ntp.org"
#define NTP_FALLBACK_IP   "216.239.35.0" /* Google, used when DNS fails */
#define NTP_TIMEOUT_MS    5000
/* Safety net on top of sntp_simple()'s own timeouts (2 x 5 s + DNS). */
#define NTP_UI_TIMEOUT_MS 20000

/* Written by the work item before it gives g_ntp_done. */
static int g_ntp_rc;
static int64_t g_ntp_secs;
static int64_t g_ntp_at_ms; /* uptime when the reply arrived */

static K_SEM_DEFINE(g_ntp_done, 0, 1);

static void ntp_work_handler(struct k_work *work)
{
    struct sntp_time st;

    ARG_UNUSED(work);
    int r = sntp_simple(NTP_SERVER, NTP_TIMEOUT_MS, &st);
    if (r == -ENOENT) {
        r = sntp_simple(NTP_FALLBACK_IP, NTP_TIMEOUT_MS, &st);
    }
    g_ntp_rc = r;
    if (r == 0) {
        g_ntp_secs = (int64_t)st.seconds;
        g_ntp_at_ms = k_uptime_get();
    }
    k_sem_give(&g_ntp_done);
}

static K_WORK_DEFINE(g_ntp_work, ntp_work_handler);
#endif /* CONFIG_SNTP */

static void ntp_start(void)
{
#if defined(CONFIG_SNTP)
    /* A request from an earlier, cancelled wait may still be in flight —
     * just wait for that one.  Otherwise drop whatever result such a request
     * left behind and start a fresh one. */
    if (!k_work_busy_get(&g_ntp_work)) {
        k_sem_reset(&g_ntp_done);
        k_work_submit(&g_ntp_work);
    }
    g_dt.syncing = true;
    g_dt.deadline = k_uptime_get() + NTP_UI_TIMEOUT_MS;
    update_hints();
    screen_invalidate();
#else
    overlay_toast(TOAST_ICON_INFO, "NTP unavailable", "Not in this build", 2000);
#endif
}

/* @return true once the sync finished (either way) — repaint. */
static bool ntp_poll(void)
{
#if defined(CONFIG_SNTP)
    char tbuf[32];

    if (k_sem_take(&g_ntp_done, K_NO_WAIT) == 0) {
        g_dt.syncing = false;
        update_hints();
        if (g_ntp_rc == 0) {
            int64_t epoch = g_ntp_secs + (k_uptime_get() - g_ntp_at_ms) / 1000;

            akira_time_set_epoch(epoch);
            /* Refresh edit fields from newly synced time */
            load_fields(epoch);
            epoch_to_str(epoch, g_dt.tz_s, tbuf, sizeof(tbuf));
            LOG_INF("SNTP sync OK: epoch %lld", (long long)epoch);
            overlay_toast(TOAST_ICON_OK, "Time synced", tbuf, 2000);
        } else {
            LOG_ERR("SNTP: %d", g_ntp_rc);
            overlay_toast(TOAST_ICON_ERROR, "Time sync failed",
                          "Check the Wi-Fi connection", 2500);
        }
        return true;
    }
    if (k_uptime_get() >= g_dt.deadline) {
        LOG_WRN("SNTP: no answer in %d ms", NTP_UI_TIMEOUT_MS);
        g_dt.syncing = false;
        update_hints();
        overlay_toast(TOAST_ICON_ERROR, "Time sync timed out", NULL, 2500);
        return true;
    }
#endif
    return false;
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

static void item_value(int i, char *buf, size_t len)
{
    switch (i) {
    case ITEM_TZ:    snprintf(buf, len, "UTC%+d", (int)(g_dt.tz_s / 3600)); break;
    case ITEM_YEAR:  snprintf(buf, len, "%04d", g_dt.year);  break;
    case ITEM_MONTH: snprintf(buf, len, "%02d", g_dt.month); break;
    case ITEM_DAY:   snprintf(buf, len, "%02d", g_dt.day);   break;
    case ITEM_HOUR:  snprintf(buf, len, "%02d", g_dt.hour);  break;
    case ITEM_MIN:   snprintf(buf, len, "%02d", g_dt.min);   break;
    default:         buf[0] = '\0'; break;
    }
}

static void draw_row(int i, int y, bool inverted)
{
    uint16_t fg = inverted ? AKIRA_UI_INK : AKIRA_UI_PAPER;
    int ty = y + (CARD_H - 10) / 2;
    int rx = ROW_X + ROW_W - 12;
    char val[16];

    akira_display_text(ROW_X + 12, ty, g_labels[i], fg);

    item_value(i, val, sizeof(val));
    if (!val[0]) {
        return;
    }
    int vw = akira_ui_text_w(val, AKIRA_UI_FONT_SMALL);

    if (inverted && is_adjustable(i)) {
        /* < value > while focused: LEFT/RIGHT adjust it */
        akira_display_text(rx - 8, ty, ">", fg);
        akira_display_text(rx - 16 - vw, ty, val, fg);
        akira_display_text(rx - 28 - vw, ty, "<", fg);
    } else {
        akira_display_text(rx - vw, ty, val, fg);
    }
}

static void draw_clock_panel(void)
{
    char tbuf[32];
    uint16_t fg = AKIRA_UI_PAPER;

    if (akira_time_is_set()) {
        epoch_to_str(akira_time_get_epoch(), g_dt.tz_s, tbuf, sizeof(tbuf));
    } else {
        snprintf(tbuf, sizeof(tbuf), "Clock not set");
        fg = SHELL_C_GRAY;
    }
    akira_display_rounded_rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 6, SHELL_C_GRAY);
    akira_display_text_large(PANEL_X + (PANEL_W - akira_ui_text_w(tbuf, AKIRA_UI_FONT_LARGE)) / 2,
                             PANEL_Y + (PANEL_H - 18) / 2, tbuf, fg);
}

static void draw_sync_card(void)
{
    const int w = 220, h = 64;
    int x = (SCR_W - w) / 2;
    int y = (SCREEN_CONTENT_Y + SCREEN_CONTENT_B - h) / 2;

    screen_draw_scrim();
    akira_ui_dither_card(x, y, w, h, 12, false, 4);
    akira_ui_spinner(x + 30, y + h / 2, 12, k_uptime_get());
    akira_display_text(x + 54, y + 18, "Syncing time", AKIRA_UI_PAPER);
    akira_display_text(x + 54, y + 36, "via NTP", SHELL_C_GRAY);
}

static void dt_draw(void)
{
    int scroll = anim_value(&g_dt.scroll_px);
    int hl = anim_value(&g_dt.hl);

    /* Idle cards first, then the highlight over them, then all labels. */
    int first = MAX(scroll / ROW_H - 1, 0);
    int last = MIN(first + VISIBLE_ROWS + 2, NUM_ITEMS);

    for (int i = first; i < last; i++) {
        akira_ui_dither_card(ROW_X, ROW_TOP + i * ROW_H - scroll, ROW_W, CARD_H, 8,
                             false, 2);
    }
    akira_ui_dither_card(ROW_X, ROW_TOP + hl - scroll, ROW_W, CARD_H, 8, true, 3);
    for (int i = first; i < last; i++) {
        draw_row(i, ROW_TOP + i * ROW_H - scroll, abs(hl - i * ROW_H) < ROW_H / 2);
    }

    /* Clip rows that are scrolling in or out above / below the list. */
    akira_display_rect(0, SBAR_H + 2, SCR_W, ROW_TOP - SBAR_H - 2, AKIRA_UI_INK);
    akira_display_rect(0, LIST_B, SCR_W, FOOT_Y - LIST_B, AKIRA_UI_INK);

    draw_clock_panel();

    /* Scrollbar lane */
    if (NUM_ITEMS > VISIBLE_ROWS) {
        int track_y = ROW_TOP, track_h = VISIBLE_ROWS * ROW_H - 5;
        int max_scroll = (NUM_ITEMS - VISIBLE_ROWS) * ROW_H;
        int thumb_h = MAX(track_h * VISIBLE_ROWS / NUM_ITEMS, 12);
        int thumb_y = track_y + (track_h - thumb_h) * CLAMP(scroll, 0, max_scroll) / max_scroll;

        akira_display_vline(SCR_W - 9, track_y, track_h, SHELL_C_DKGRAY);
        akira_display_rounded_rect_fill(SCR_W - 11, thumb_y, 5, thumb_h, 2, AKIRA_UI_PAPER);
    }

    if (g_dt.syncing) {
        draw_sync_card();
    }
}

/* ------------------------------------------------------------------ */
/* Behaviour                                                           */
/* ------------------------------------------------------------------ */

static void move_sel(int to)
{
    to = CLAMP(to, 0, NUM_ITEMS - 1);
    if (to == g_dt.sel) {
        return;
    }
    g_dt.sel = to;
    anim_to(&g_dt.hl, to * ROW_H, SEL_MS, ANIM_EASE_OUT);

    int scroll = g_dt.scroll;

    if (to < scroll) {
        scroll = to;
    } else if (to >= scroll + VISIBLE_ROWS) {
        scroll = to - VISIBLE_ROWS + 1;
    }
    if (scroll != g_dt.scroll) {
        g_dt.scroll = scroll;
        anim_to(&g_dt.scroll_px, scroll * ROW_H, SEL_MS + 40, ANIM_EASE_OUT);
    }
    update_hints();
    screen_invalidate();
}

/* LEFT / RIGHT: adjust the selected spinner field */
static void adjust(int dir)
{
    switch (g_dt.sel) {
    case ITEM_TZ: {
        int32_t tz = g_dt.tz_s + dir * 3600;

        if (tz < -12 * 3600 || tz > 14 * 3600) {
            return;
        }
        g_dt.tz_s = tz;
        akira_time_set_tz_offset_s(tz);
        break;
    }
    case ITEM_YEAR:
        g_dt.year = CLAMP(g_dt.year + dir, 2000, 2099);
        clamp_day();
        break;
    case ITEM_MONTH:
        g_dt.month += dir;
        if (g_dt.month < 1)  g_dt.month = 12;
        if (g_dt.month > 12) g_dt.month = 1;
        clamp_day();
        break;
    case ITEM_DAY:
        g_dt.day += dir;
        clamp_day();
        break;
    case ITEM_HOUR:
        g_dt.hour = (g_dt.hour + dir + 24) % 24;
        break;
    case ITEM_MIN:
        g_dt.min = (g_dt.min + dir + 60) % 60;
        break;
    default:
        return;
    }
    screen_invalidate();
}

static void set_clock(void)
{
    char tbuf[24];
    int64_t new_epoch = compose_epoch(g_dt.year, g_dt.month, g_dt.day,
                                      g_dt.hour, g_dt.min, 0, g_dt.tz_s);

    akira_time_set_epoch(new_epoch);
    LOG_INF("Clock set to %04d-%02d-%02d %02d:%02d via settings",
            g_dt.year, g_dt.month, g_dt.day, g_dt.hour, g_dt.min);
    snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d",
             g_dt.year, g_dt.month, g_dt.day, g_dt.hour, g_dt.min);
    overlay_toast(TOAST_ICON_OK, "Clock set", tbuf, 2000);
}

static void dt_key(uint32_t just)
{
    if (g_dt.syncing) {
        /* Stop waiting; a late answer is discarded by the next ntp_start(). */
        if (just & BIT(AKIRA_BTN_B)) {
            g_dt.syncing = false;
            update_hints();
            screen_invalidate();
        }
        return;
    }
    if (just & BIT(AKIRA_BTN_B)) {
        screen_pop();
        return;
    }
    if (just & BIT(AKIRA_BTN_UP)) {
        move_sel(g_dt.sel - 1);
    }
    if (just & BIT(AKIRA_BTN_DOWN)) {
        move_sel(g_dt.sel + 1);
    }
    if (just & BIT(AKIRA_BTN_LEFT)) {
        adjust(-1);
    }
    if (just & BIT(AKIRA_BTN_RIGHT)) {
        adjust(+1);
    }
    if (just & BIT(AKIRA_BTN_A)) {
        switch (g_dt.sel) {
        case ITEM_NTP:
            ntp_start();
            break;
        case ITEM_SET:
            set_clock();
            break;
        case ITEM_BACK:
            screen_pop();
            return;
        default:
            break;
        }
    }
}

static bool dt_tick(void)
{
    bool redraw = anim_running(&g_dt.hl) || anim_running(&g_dt.scroll_px);

    if (g_dt.syncing) {
        if (ntp_poll()) {
            return true;
        }
        int64_t phase = k_uptime_get() / SPIN_STEP_MS;

        if (phase != g_dt.spin_phase) {
            g_dt.spin_phase = phase;
            redraw = true;
        }
    }
    return redraw;
}

static void dt_status(void)
{
    screen_invalidate(); /* the clock panel shows seconds */
}

static void dt_enter(void *arg)
{
    ARG_UNUSED(arg);
    memset(&g_dt, 0, sizeof(g_dt));
    anim_set(&g_dt.hl, 0);
    anim_set(&g_dt.scroll_px, 0);

    g_dt.tz_s = akira_time_get_tz_offset_s();

    /* Initialise edit fields from current local time */
    if (akira_time_is_set()) {
        load_fields(akira_time_get_epoch());
    } else {
        /* If epoch isn't set, default to a sensible base */
        g_dt.year = 2025; g_dt.month = 1; g_dt.day = 1;
        g_dt.hour = 0;    g_dt.min = 0;
    }
    update_hints();
}

static void dt_leave(void)
{
    /* Popped mid-sync (B or HOME): stop waiting.  The work item finishes on
     * its own and its result is dropped by the next ntp_start(). */
    g_dt.syncing = false;
}

static const shell_screen_t datetime_screen = {
    .name = "datetime",
    .title = "Date & time",
    .flags = SCR_F_STATUSBAR | SCR_F_FOOTER,
    .hint_l = "A:Sync",
    .hint_r = "B:Back",
    .enter = dt_enter,
    .leave = dt_leave,
    .on_key = dt_key,
    .tick = dt_tick,
    .draw = dt_draw,
    .on_status = dt_status,
};

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

void datetime_open(void)
{
    screen_push(&datetime_screen, NULL);
}
