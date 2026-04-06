/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_datetime_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_datetime_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file datetime_screen.c
 * @brief Date and time display, SNTP sync button, and manual rollers.
 */

#include <zephyr/kernel.h>
#include <time.h>


#include "../shell_theme.h"
#include "datetime_screen.h"

#if defined(CONFIG_SNTP)
#include <zephyr/net/sntp.h>
#endif

static lv_obj_t *g_screen;
static lv_obj_t *g_clock_label;
static lv_obj_t *g_sync_btn;

/* Rollers for manual entry */
static lv_obj_t *g_rol_year;
static lv_obj_t *g_rol_month;
static lv_obj_t *g_rol_day;
static lv_obj_t *g_rol_hour;
static lv_obj_t *g_rol_min;

/* Clock refresh timer */
static lv_timer_t *g_clock_timer;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void refresh_clock_label(void)
{
    if (!g_clock_label) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    gmtime_r(&ts.tv_sec, &t);
    lv_label_set_text_fmt(g_clock_label,
        "%04d-%02d-%02d  %02d:%02d:%02d UTC",
        1900 + t.tm_year, 1 + t.tm_mon, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
}

static void clock_timer_cb(lv_timer_t *timer)
{
    ARG_UNUSED(timer);
    refresh_clock_label();
}

/* ------------------------------------------------------------------ */
/* SNTP sync                                                            */
/* ------------------------------------------------------------------ */

static void sync_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

#if defined(CONFIG_SNTP)
    struct sntp_time sntp_ts;
    int ret = sntp_simple("pool.ntp.org", 5000, &sntp_ts);
    if (ret < 0) {
        LOG_ERR("SNTP sync failed: %d", ret);
        lv_label_set_text(lv_obj_get_child(g_sync_btn, 0),
                          "Sync failed");
        return;
    }

    struct timespec ts = { .tv_sec = (time_t)sntp_ts.seconds };
    clock_settime(CLOCK_REALTIME, &ts);
    LOG_INF("SNTP synced: %llu", (unsigned long long)sntp_ts.seconds);
    lv_label_set_text(lv_obj_get_child(g_sync_btn, 0), "Synced!");
#else
    lv_label_set_text(lv_obj_get_child(g_sync_btn, 0), "SNTP N/A");
#endif

    refresh_clock_label();
}

/* ------------------------------------------------------------------ */
/* Apply manual rollers                                                 */
/* ------------------------------------------------------------------ */

static void apply_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    struct tm t = { 0 };
    t.tm_year = lv_roller_get_selected(g_rol_year) + (2024 - 1900);
    t.tm_mon  = lv_roller_get_selected(g_rol_month);
    t.tm_mday = lv_roller_get_selected(g_rol_day) + 1;
    t.tm_hour = lv_roller_get_selected(g_rol_hour);
    t.tm_min  = lv_roller_get_selected(g_rol_min);

    time_t epoch = mktime(&t);
    struct timespec ts = { .tv_sec = epoch };
    clock_settime(CLOCK_REALTIME, &ts);
    LOG_INF("Manual time applied");
    refresh_clock_label();
}

/* ------------------------------------------------------------------ */
/* Back key                                                             */
/* ------------------------------------------------------------------ */

static void back_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_KEY) {
        uint32_t key = lv_indev_get_key(lv_indev_get_act());
        if (key == LV_KEY_ESC) {
            if (g_clock_timer) {
                lv_timer_del(g_clock_timer);
                g_clock_timer = NULL;
            }
            extern void settings_screen_load(void);
            settings_screen_load();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Roller helpers (compact options string)                             */
/* ------------------------------------------------------------------ */

static lv_obj_t *make_roller(lv_obj_t *parent, const char *opts,
                               int x, int y)
{
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 2);
    lv_obj_set_width(r, 48);
    lv_obj_align(r, LV_ALIGN_TOP_LEFT, x, y);
    return r;
}

/* ------------------------------------------------------------------ */
/* Screen construction                                                  */
/* ------------------------------------------------------------------ */

static void build_screen(void)
{
    g_screen = lv_obj_create(NULL);
    lv_obj_add_style(g_screen, &g_style_screen, 0);

    shell_theme_make_header(g_screen, "Date & Time");
    shell_theme_make_footer(g_screen, "B:Back", "");

    int y = SHELL_HEADER_H + 8;

    /* Current time */
    g_clock_label = lv_label_create(g_screen);
    lv_label_set_text(g_clock_label, "----");
    lv_obj_set_style_text_font(g_clock_label, SHELL_FONT_SMALL, 0);
    lv_obj_align(g_clock_label, LV_ALIGN_TOP_LEFT, 8, y);
    refresh_clock_label();

    g_clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    y += 22;

    /* SNTP sync button */
    g_sync_btn = lv_btn_create(g_screen);
    lv_obj_set_size(g_sync_btn, 120, 30);
    lv_obj_align(g_sync_btn, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_event_cb(g_sync_btn, sync_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sync_lbl = lv_label_create(g_sync_btn);
    lv_label_set_text(sync_lbl, "Sync NTP");
    lv_obj_center(sync_lbl);
    y += 38;

    /* Separator */
    lv_obj_t *sep = lv_obj_create(g_screen);
    lv_obj_set_size(sep, SHELL_SCREEN_W - 16, 1);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_style(sep, &g_style_separator, 0);
    y += 8;

    /* Manual entry header */
    lv_obj_t *man_lbl = lv_label_create(g_screen);
    lv_label_set_text(man_lbl, "Manual (Y/M/D  H:M)");
    lv_obj_set_style_text_font(man_lbl, SHELL_FONT_SMALL, 0);
    lv_obj_align(man_lbl, LV_ALIGN_TOP_LEFT, 8, y);
    y += 18;

    /* Year roller — 2024-2034 */
    static const char year_opts[] =
        "2024\n2025\n2026\n2027\n2028\n2029\n2030\n2031\n2032\n2033\n2034";
    g_rol_year  = make_roller(g_screen, year_opts, 8, y);

    /* Month roller */
    static const char month_opts[] =
        "Jan\nFeb\nMar\nApr\nMay\nJun\nJul\nAug\nSep\nOct\nNov\nDec";
    g_rol_month = make_roller(g_screen, month_opts, 60, y);

    /* Day roller 01-31 */
    static const char day_opts[] =
        "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n"
        "16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31";
    g_rol_day   = make_roller(g_screen, day_opts, 112, y);

    /* Hour roller 00-23 */
    static const char hour_opts[] =
        "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n"
        "12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23";
    g_rol_hour  = make_roller(g_screen, hour_opts, 176, y);

    /* Min roller 00-59 */
    static const char min_opts[] =
        "00\n05\n10\n15\n20\n25\n30\n35\n40\n45\n50\n55";
    g_rol_min   = make_roller(g_screen, min_opts, 228, y);

    y += 70;

    /* Apply button */
    lv_obj_t *apply_btn = lv_btn_create(g_screen);
    lv_obj_set_size(apply_btn, 100, 30);
    lv_obj_align(apply_btn, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_event_cb(apply_btn, apply_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *apply_lbl = lv_label_create(apply_btn);
    lv_label_set_text(apply_lbl, "Apply");
    lv_obj_center(apply_lbl);

    lv_obj_add_event_cb(g_screen, back_event_cb, LV_EVENT_KEY, NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void datetime_screen_load(void)
{
    if (!g_screen) {
        build_screen();
    }
    lv_scr_load(g_screen);
}
