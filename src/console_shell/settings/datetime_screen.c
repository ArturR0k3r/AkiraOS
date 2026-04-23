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

/* Decompose UTC epoch + tz_offset into display string */
static void epoch_to_str(int64_t epoch, int32_t tz_s, char *buf, size_t len)
{
    int64_t local = epoch + tz_s;
    int64_t day_sec = local % 86400;
    if (day_sec < 0) day_sec += 86400;
    int64_t days = local / 86400;
    if (local < 0 && day_sec != 0) days--;
    int h  = (int)(day_sec / 3600);
    int mi = (int)((day_sec % 3600) / 60);
    int s  = (int)(day_sec % 60);
    /* Gregorian decomposition */
    int y = 1970;
    while (1) {
        int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        int ydays = leap ? 366 : 365;
        if (days < ydays) break;
        days -= ydays; y++;
    }
    static const int8_t md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    int mo = 1;
    for (; mo < 12; mo++) {
        int mdays = (mo == 2 && leap) ? 29 : md[mo - 1];
        if (days < mdays) break;
        days -= mdays;
    }
    int d = (int)days + 1;
    snprintf(buf, len, "%04d-%02d-%02d  %02d:%02d:%02d", y, mo, d, h, mi, s);
}

#define NUM_ITEMS 3
static int32_t g_tz_edit_s; /* working copy while on this screen */

static void draw_item(int idx, int sel, const char *lbl, const char *rv)
{
    int bx = SS_MENU_X;
    int by = SS_CONT_Y + 38 + idx * SS_MENU_ITH + 2; /* offset past clock panel */
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
    int tz_h = (int)(g_tz_edit_s / 3600);
    snprintf(tz_label, sizeof(tz_label), "UTC%+d", tz_h);

    akira_display_clear(SS_C_BLACK);
    ss_draw_header("DATE & TIME");
    akira_display_rect(0, SS_CONT_Y, SS_SCR_W, SS_RIB_Y - SS_CONT_Y, SS_C_BLACK);

    /* Clock display panel */
    int px = SS_MENU_X, py = SS_CONT_Y + 4;
    int pw = SS_MENU_W, ph = 26;
    akira_display_rounded_rect_fill(px, py, pw, ph, 4, SS_C_BLACK);
    akira_display_rounded_rect(px, py, pw, ph, 4, SS_C_DKGRAY);
    ss_draw_centred(px, py + 8, pw, tbuf, SS_C_WHITE, SS_C_BLACK);

    /* Menu items */
    draw_item(0, sel, "Sync NTP", "");
    draw_item(1, sel, "UTC Offset", tz_label);
    draw_item(2, sel, "Back", "");

    ss_draw_ribbon("[</> Offset  [A] Select", "[B] Back");
    akira_display_flush();
}

void datetime_screen_load(void)
{
    extern void settings_screen_load(void);
    g_tz_edit_s = akira_time_get_tz_offset_s();
    int sel = 0;
    draw(sel);
    uint32_t prev = akira_input_get_bitmask();
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns = akira_input_get_bitmask(), just = btns & ~prev;
        prev = btns;
        if (!just) continue;

        if (just & BIT(AKIRA_BTN_UP))   { if (sel > 0)            { sel--; draw(sel); } }
        if (just & BIT(AKIRA_BTN_DOWN)) { if (sel < NUM_ITEMS - 1) { sel++; draw(sel); } }

        /* LEFT / RIGHT adjust UTC offset when on that row */
        if (sel == 1) {
            if (just & BIT(AKIRA_BTN_LEFT)) {
                if (g_tz_edit_s > -12 * 3600) { g_tz_edit_s -= 3600; }
                akira_time_set_tz_offset_s(g_tz_edit_s);
                draw(sel);
            }
            if (just & BIT(AKIRA_BTN_RIGHT)) {
                if (g_tz_edit_s < 14 * 3600) { g_tz_edit_s += 3600; }
                akira_time_set_tz_offset_s(g_tz_edit_s);
                draw(sel);
            }
        }

        if (just & BIT(AKIRA_BTN_A)) {
            if (sel == 0) {
#if defined(CONFIG_SNTP)
                struct sntp_time st;
                int r = sntp_simple("pool.ntp.org", 5000, &st);
                if (r == -ENOENT) {
                    r = sntp_simple("216.239.35.0", 5000, &st);
                }
                if (!r) {
                    struct timespec ts2 = { (time_t)st.seconds, 0 };
                    clock_settime(CLOCK_REALTIME, &ts2);
                    akira_time_set_epoch((int64_t)st.seconds);
                } else {
                    LOG_ERR("SNTP: %d", r);
                }
#endif
                draw(sel);
            } else if (sel == 2) {
                settings_screen_load();
                return;
            }
            /* sel==1 (UTC Offset): A press does nothing extra */
        }
        if ((just & BIT(AKIRA_BTN_B)) || (just & BIT(AKIRA_BTN_HOME))) {
            settings_screen_load();
            return;
        }
    }
}
