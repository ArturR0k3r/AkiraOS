/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_datetime_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_datetime_screen, CONFIG_AKIRA_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <lib/akira_time.h>
#include "datetime_screen.h"
#include "../settings_shared.h"

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

static int32_t g_tz_edit_s;
static int g_edit_year;
static int g_edit_month;
static int g_edit_day;
static int g_edit_hour;
static int g_edit_min;

static void clamp_day(void)
{
    int mx = days_in_month(g_edit_month, g_edit_year);
    if (g_edit_day > mx) g_edit_day = mx;
    if (g_edit_day < 1)  g_edit_day = 1;
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

static void draw_item(int idx, int sel, const char *lbl, const char *rv)
{
    int bx = SS_MENU_X;
    int by = SS_CONT_Y + 38 + idx * SS_MENU_ITH + 2;
    int bw = SS_MENU_W;
    int bh = SS_MENU_ITH - 4;
    bool hi = (idx == sel);

    if (hi) {
        ss_glass_rect_focus(bx, by, bw, bh, 5);
    } else {
        ss_glass_rect_dim(bx, by, bw, bh, 5);
    }
    int ty = by + (bh - 10) / 2;
    uint16_t fg = hi ? SS_C_WHITE : SS_C_DKGRAY;
    akira_display_text(bx + 10, ty, lbl, fg);
    if (rv && rv[0]) {
        int rvlen = (int)strlen(rv);
        akira_display_text(bx + bw - rvlen * 8 - 10, ty, rv, fg);
    }
}

static void draw(int sel)
{
    int64_t epoch = akira_time_get_epoch();
    char tbuf[32];
    if (akira_time_is_set()) {
        epoch_to_str(epoch, g_tz_edit_s, tbuf, sizeof(tbuf));
    } else {
        snprintf(tbuf, sizeof(tbuf), "Clock not set");
    }

    char tz_label[16];
    snprintf(tz_label, sizeof(tz_label), "UTC%+d", (int)(g_tz_edit_s / 3600));

    char yr_s[8], mo_s[4], dy_s[4], hr_s[4], mn_s[4];
    snprintf(yr_s, sizeof(yr_s), "%04d", g_edit_year);
    snprintf(mo_s, sizeof(mo_s), "%02d",  g_edit_month);
    snprintf(dy_s, sizeof(dy_s), "%02d",  g_edit_day);
    snprintf(hr_s, sizeof(hr_s), "%02d",  g_edit_hour);
    snprintf(mn_s, sizeof(mn_s), "%02d",  g_edit_min);

    akira_display_clear(SS_C_BLACK);
    ss_draw_header("DATE & TIME");
    akira_display_rect(0, SS_CONT_Y, SS_SCR_W, SS_RIB_Y - SS_CONT_Y, SS_C_BLACK);

    /* Current clock panel */
    int px = SS_MENU_X, py = SS_CONT_Y + 4;
    int pw = SS_MENU_W, ph = 26;
    akira_display_rounded_rect_fill(px, py, pw, ph, 4, SS_C_BLACK);
    akira_display_rounded_rect(px, py, pw, ph, 4, SS_C_DKGRAY);
    ss_draw_centred(px, py + 8, pw, tbuf, SS_C_WHITE, SS_C_BLACK);

    /* Scrollable item list — items above SS_RIB_Y */
    int vis = (SS_RIB_Y - (SS_CONT_Y + 38 + 2)) / SS_MENU_ITH;
    if (vis < 1) vis = 1;

    /* Compute scroll so sel is always visible */
    static int scroll;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + vis) scroll = sel - vis + 1;
    if (scroll < 0) scroll = 0;
    if (scroll > NUM_ITEMS - vis) scroll = NUM_ITEMS - vis;

    const char *labels[NUM_ITEMS] = {
        "Sync NTP", "UTC Offset",
        "Year", "Month", "Day", "Hour", "Minute",
        "Set Clock", "Back"
    };
    const char *rvals[NUM_ITEMS] = {
        "", tz_label,
        yr_s, mo_s, dy_s, hr_s, mn_s,
        "", ""
    };

    for (int i = scroll; i < NUM_ITEMS && i < scroll + vis; i++) {
        /* Re-map draw position relative to scroll */
        int display_idx = i - scroll;
        int bx = SS_MENU_X;
        int by = SS_CONT_Y + 38 + display_idx * SS_MENU_ITH + 2;
        int bw = SS_MENU_W;
        int bh = SS_MENU_ITH - 4;
        bool hi = (i == sel);

        if (hi) {
            ss_glass_rect_focus(bx, by, bw, bh, 5);
        } else {
            ss_glass_rect_dim(bx, by, bw, bh, 5);
        }
        int ty = by + (bh - 10) / 2;
        uint16_t fg = hi ? SS_C_WHITE : SS_C_DKGRAY;
        akira_display_text(bx + 10, ty, labels[i], fg);
        if (rvals[i] && rvals[i][0]) {
            int rvlen = (int)strlen(rvals[i]);
            akira_display_text(bx + bw - rvlen * 8 - 10, ty, rvals[i], fg);
        }
    }

    /* Scrollbar */
    if (NUM_ITEMS > vis) {
        int sbar_x = SS_SCR_W - 8;
        int sbar_w = 5;
        int track_y = SS_CONT_Y + 38 + 2;
        int track_h = vis * SS_MENU_ITH - 4;
        akira_display_rect(sbar_x, track_y, sbar_w, track_h, SS_C_BLACK);
        int thumb_h = track_h * vis / NUM_ITEMS;
        if (thumb_h < 8) thumb_h = 8;
        int max_off = NUM_ITEMS - vis;
        int thumb_y = track_y + (max_off > 0 ? (track_h - thumb_h) * scroll / max_off : 0);
        akira_display_rect(sbar_x + 1, thumb_y, sbar_w - 2, thumb_h, SS_C_WHITE);
    }

    ss_draw_ribbon("[</> Adjust", "[B] Back");
    akira_display_flush();
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

void datetime_screen_load(void)
{
    extern void settings_screen_load(void);

    g_tz_edit_s = akira_time_get_tz_offset_s();

    /* Initialise edit fields from current local time */
    {
        int64_t epoch = akira_time_get_epoch();
        int s;
        epoch_decompose(epoch, g_tz_edit_s,
                        &g_edit_year, &g_edit_month, &g_edit_day,
                        &g_edit_hour, &g_edit_min, &s);
        /* If epoch isn't set, default to a sensible base */
        if (!akira_time_is_set()) {
            g_edit_year = 2025; g_edit_month = 1; g_edit_day = 1;
            g_edit_hour = 0;    g_edit_min = 0;
        }
    }

    int sel = 0;
    draw(sel);
    uint32_t prev = akira_input_get_bitmask();

    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns = akira_input_get_bitmask(), just = btns & ~prev;
        prev = btns;
        if (!just) continue;

        if (just & BIT(AKIRA_BTN_UP)) {
            if (sel > 0) { sel--; draw(sel); }
        }
        if (just & BIT(AKIRA_BTN_DOWN)) {
            if (sel < NUM_ITEMS - 1) { sel++; draw(sel); }
        }

        /* LEFT / RIGHT: adjust the selected spinner field */
        if (just & BIT(AKIRA_BTN_LEFT)) {
            switch (sel) {
            case ITEM_TZ:
                if (g_tz_edit_s > -12 * 3600) g_tz_edit_s -= 3600;
                akira_time_set_tz_offset_s(g_tz_edit_s);
                break;
            case ITEM_YEAR:  g_edit_year--;  if (g_edit_year < 2000) g_edit_year = 2000; clamp_day(); break;
            case ITEM_MONTH: g_edit_month--; if (g_edit_month < 1)   g_edit_month = 12;  clamp_day(); break;
            case ITEM_DAY:   g_edit_day--;   clamp_day(); break;
            case ITEM_HOUR:  g_edit_hour--;  if (g_edit_hour < 0)    g_edit_hour = 23;   break;
            case ITEM_MIN:   g_edit_min--;   if (g_edit_min  < 0)    g_edit_min  = 59;   break;
            default: break;
            }
            draw(sel);
        }
        if (just & BIT(AKIRA_BTN_RIGHT)) {
            switch (sel) {
            case ITEM_TZ:
                if (g_tz_edit_s < 14 * 3600) g_tz_edit_s += 3600;
                akira_time_set_tz_offset_s(g_tz_edit_s);
                break;
            case ITEM_YEAR:  g_edit_year++;  if (g_edit_year > 2099) g_edit_year = 2099; clamp_day(); break;
            case ITEM_MONTH: g_edit_month++; if (g_edit_month > 12)  g_edit_month = 1;   clamp_day(); break;
            case ITEM_DAY:   g_edit_day++;   clamp_day(); break;
            case ITEM_HOUR:  g_edit_hour++;  if (g_edit_hour > 23)   g_edit_hour = 0;    break;
            case ITEM_MIN:   g_edit_min++;   if (g_edit_min  > 59)   g_edit_min  = 0;    break;
            default: break;
            }
            draw(sel);
        }

        if (just & BIT(AKIRA_BTN_A)) {
            switch (sel) {
            case ITEM_NTP:
#if defined(CONFIG_SNTP)
                {
                    struct sntp_time st;
                    int r = sntp_simple("pool.ntp.org", 5000, &st);
                    if (r == -ENOENT)
                        r = sntp_simple("216.239.35.0", 5000, &st);
                    if (!r) {
                        akira_time_set_epoch((int64_t)st.seconds);
                        /* Refresh edit fields from newly synced time */
                        int s2;
                        epoch_decompose((int64_t)st.seconds, g_tz_edit_s,
                                        &g_edit_year, &g_edit_month, &g_edit_day,
                                        &g_edit_hour, &g_edit_min, &s2);
                    } else {
                        LOG_ERR("SNTP: %d", r);
                    }
                }
#endif
                draw(sel);
                break;

            case ITEM_SET:
                {
                    int64_t new_epoch = compose_epoch(
                        g_edit_year, g_edit_month, g_edit_day,
                        g_edit_hour, g_edit_min, 0,
                        g_tz_edit_s);
                    akira_time_set_epoch(new_epoch);
                    LOG_INF("Clock set to %04d-%02d-%02d %02d:%02d via settings",
                            g_edit_year, g_edit_month, g_edit_day,
                            g_edit_hour, g_edit_min);
                }
                draw(sel);
                break;

            case ITEM_BACK:
                settings_screen_load();
                return;

            default:
                break;
            }
        }

        if ((just & BIT(AKIRA_BTN_B)) || (just & BIT(AKIRA_BTN_HOME))) {
            settings_screen_load();
            return;
        }
    }
}
