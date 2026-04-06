/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_wifi_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_wifi_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file wifi_screen.c
 * @brief WiFi network list, password entry, and connection management.
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>


#include "../shell_theme.h"
#include "../home_screen.h"
#include "wifi_screen.h"

#define WIFI_MAX_NETWORKS  16
#define WIFI_SSID_MAX_LEN  33
#define WIFI_PASS_MAX_LEN  64

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

static struct wifi_scan_result g_scan_results[WIFI_MAX_NETWORKS];
static int                     g_scan_count;
static struct k_sem             g_scan_done;

static void on_wifi_mgmt_event(struct net_mgmt_event_callback *cb,
                                uint32_t mgmt_event, struct net_if *iface)
{
    if (mgmt_event == NET_EVENT_WIFI_SCAN_RESULT) {
        const struct wifi_scan_result *res =
            (const struct wifi_scan_result *)cb->info;
        if (g_scan_count < WIFI_MAX_NETWORKS) {
            g_scan_results[g_scan_count++] = *res;
        }
    } else if (mgmt_event == NET_EVENT_WIFI_SCAN_DONE) {
        k_sem_give(&g_scan_done);
    }
}
#endif /* CONFIG_WIFI */

/* ------------------------------------------------------------------ */
/* Screen objects                                                       */
/* ------------------------------------------------------------------ */

static lv_obj_t *g_screen;
static lv_obj_t *g_list;
static lv_obj_t *g_status_label;
static lv_obj_t *g_kb;

static char g_selected_ssid[WIFI_SSID_MAX_LEN];

static void show_status(const char *msg)
{
    if (g_status_label) {
        lv_label_set_text(g_status_label, msg);
    }
}

static void back_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_KEY) {
        uint32_t key = lv_indev_get_key(lv_indev_get_act());
        if (key == LV_KEY_ESC) {
            extern void settings_screen_load(void);
            settings_screen_load();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Password keyboard                                                    */
/* ------------------------------------------------------------------ */

static void connect_with_password(const char *ssid, const char *pass)
{
    LOG_INF("Connecting to SSID: %s", ssid);

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
    struct net_if *iface = net_if_get_default();
    struct wifi_connect_req_params params = {
        .ssid        = (const uint8_t *)ssid,
        .ssid_length = (uint8_t)strlen(ssid),
        .psk         = (const uint8_t *)pass,
        .psk_length  = (uint8_t)strlen(pass),
        .security    = WIFI_SECURITY_TYPE_PSK,
        .channel     = WIFI_CHANNEL_ANY,
    };

    if (strlen(pass) == 0) {
        params.security = WIFI_SECURITY_TYPE_NONE;
    }

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params,
                       sizeof(params));
    if (ret < 0) {
        LOG_ERR("WiFi connect request failed: %d", ret);
        show_status("Connection failed");
        return;
    }

    /* Persist credentials */
    settings_save_one("akira/wifi/ssid", ssid, strlen(ssid));
    settings_save_one("akira/wifi/pass", pass, strlen(pass));
#endif

    show_status("Connecting…");
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_keyboard_get_textarea(g_kb);

    if (code == LV_EVENT_READY) {
        const char *pass = lv_textarea_get_text(ta);
        connect_with_password(g_selected_ssid, pass);
        lv_obj_del(lv_obj_get_parent(g_kb)); /* remove password panel */
        g_kb = NULL;
    } else if (code == LV_EVENT_CANCEL) {
        lv_obj_del(lv_obj_get_parent(g_kb));
        g_kb = NULL;
    }
}

static void open_password_entry(const char *ssid)
{
    strncpy(g_selected_ssid, ssid, sizeof(g_selected_ssid) - 1);
    g_selected_ssid[sizeof(g_selected_ssid) - 1] = '\0';

    /* Panel that covers the content area */
    lv_obj_t *panel = lv_obj_create(g_screen);
    lv_obj_set_size(panel, SHELL_SCREEN_W,
                    SHELL_SCREEN_H - SHELL_HEADER_H - SHELL_FOOTER_H);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);

    lv_obj_t *lbl = lv_label_create(panel);
    lv_label_set_text_fmt(lbl, "Password for: %s", ssid);
    lv_obj_add_style(lbl, &g_style_list_item, 0);
    lv_obj_set_width(lbl, SHELL_SCREEN_W - 8);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *ta = lv_textarea_create(panel);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, SHELL_SCREEN_W - 8);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 24);

    g_kb = lv_keyboard_create(panel);
    lv_keyboard_set_textarea(g_kb, ta);
    lv_obj_align(g_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(g_kb, kb_event_cb, LV_EVENT_ALL, NULL);
}

/* ------------------------------------------------------------------ */
/* SSID list entry click                                                */
/* ------------------------------------------------------------------ */

static void ssid_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *btn = lv_event_get_target(e);
    const char *ssid = lv_list_get_btn_text(g_list, btn);
    open_password_entry(ssid);
}

/* ------------------------------------------------------------------ */
/* Populate list                                                        */
/* ------------------------------------------------------------------ */

static void populate_list(void)
{
    lv_obj_clean(g_list);

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
    if (g_scan_count == 0) {
        lv_list_add_text(g_list, "No networks found");
        return;
    }

    for (int i = 0; i < g_scan_count; i++) {
        /* SSID may not be null-terminated */
        char ssid[WIFI_SSID_MAX_LEN];
        int len = MIN((int)g_scan_results[i].ssid_length,
                      (int)sizeof(ssid) - 1);
        memcpy(ssid, g_scan_results[i].ssid, len);
        ssid[len] = '\0';

        lv_obj_t *btn = lv_list_add_btn(g_list, NULL, ssid);
        lv_obj_add_style(btn, &g_style_list_item, 0);
        lv_obj_add_event_cb(btn, ssid_click_cb, LV_EVENT_CLICKED, NULL);
    }
#else
    lv_list_add_text(g_list, "WiFi not supported");
#endif
}

/* ------------------------------------------------------------------ */
/* Scan work item                                                       */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
static struct net_mgmt_event_callback g_wifi_cb;

static void scan_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    g_scan_count = 0;
    k_sem_reset(&g_scan_done);

    struct net_if *iface = net_if_get_default();
    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
    if (ret < 0) {
        LOG_ERR("WiFi scan failed: %d", ret);
        show_status("Scan failed");
        return;
    }

    /* Wait up to 10 s for results */
    k_sem_take(&g_scan_done, K_SECONDS(10));
    populate_list();
    show_status("Tap a network to connect");
}

static K_WORK_DEFINE(g_scan_work, scan_work_fn);
#endif

/* ------------------------------------------------------------------ */
/* Screen construction                                                  */
/* ------------------------------------------------------------------ */

static void build_screen(void)
{
    g_screen = lv_obj_create(NULL);
    lv_obj_add_style(g_screen, &g_style_screen, 0);

    shell_theme_make_header(g_screen, "WiFi");
    shell_theme_make_footer(g_screen, "B:Back", "A:Connect");

    /* Status label */
    g_status_label = lv_label_create(g_screen);
    lv_label_set_text(g_status_label, "Scanning…");
    lv_obj_set_style_text_font(g_status_label, SHELL_FONT_SMALL, 0);
    lv_obj_set_style_text_color(g_status_label,
                                lv_color_make(0x60, 0x60, 0x60), 0);
    lv_obj_set_width(g_status_label, SHELL_SCREEN_W - 8);
    lv_obj_align(g_status_label, LV_ALIGN_TOP_LEFT, 4,
                 SHELL_HEADER_H + 2);

    /* Network list */
    g_list = lv_list_create(g_screen);
    lv_obj_set_size(g_list, SHELL_SCREEN_W,
                    SHELL_CONTENT_H - 20);
    lv_obj_align(g_list, LV_ALIGN_TOP_LEFT, 0,
                 SHELL_HEADER_H + 20);
    lv_obj_set_style_border_width(g_list, 0, 0);
    lv_obj_set_style_pad_all(g_list, 0, 0);

    lv_obj_add_event_cb(g_screen, back_event_cb, LV_EVENT_KEY, NULL);

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
    /* Register event callbacks once */
    static bool wifi_cb_registered;
    if (!wifi_cb_registered) {
        k_sem_init(&g_scan_done, 0, 1);
        net_mgmt_init_event_callback(&g_wifi_cb, on_wifi_mgmt_event,
            NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE |
            NET_EVENT_WIFI_CONNECT_RESULT);
        net_mgmt_add_event_callback(&g_wifi_cb);
        wifi_cb_registered = true;
    }
    k_work_submit(&g_scan_work);
#else
    populate_list();
    show_status("WiFi not available");
#endif
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void wifi_screen_load(void)
{
    if (!g_screen) {
        build_screen();
    }
    lv_scr_load(g_screen);
}
