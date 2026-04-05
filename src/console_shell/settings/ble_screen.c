/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_ble_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ble_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file ble_screen.c
 * @brief Bluetooth on/off toggle and pairing mode button.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>

#include "../shell_theme.h"
#include "ble_screen.h"

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#endif

static lv_obj_t *g_screen;
static lv_obj_t *g_sw;
static lv_obj_t *g_pair_btn;
static lv_obj_t *g_conn_label;

/* ------------------------------------------------------------------ */
/* BT helpers                                                          */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_BT)
static struct bt_conn *g_conn;

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_WRN("BT connection failed: %d", err);
        return;
    }
    g_conn = bt_conn_ref(conn);
    if (g_conn_label) {
        char addr[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
        lv_label_set_text_fmt(g_conn_label, "Connected: %s", addr);
    }
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(reason);
    if (g_conn) {
        bt_conn_unref(g_conn);
        g_conn = NULL;
    }
    if (g_conn_label) {
        lv_label_set_text(g_conn_label, "Not connected");
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = connected_cb,
    .disconnected = disconnected_cb,
};
#endif /* CONFIG_BT */

/* ------------------------------------------------------------------ */
/* Toggle callback                                                      */
/* ------------------------------------------------------------------ */

static void sw_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    bool on = lv_obj_has_state(g_sw, LV_STATE_CHECKED);

#if defined(CONFIG_BT)
    if (on) {
        int ret = bt_enable(NULL);
        if (ret < 0 && ret != -EALREADY) {
            LOG_ERR("bt_enable failed: %d", ret);
        }
    } else {
        int ret = bt_disable();
        if (ret < 0) {
            LOG_ERR("bt_disable failed: %d", ret);
        }
    }
#else
    ARG_UNUSED(on);
#endif

    if (g_pair_btn) {
        lv_obj_set_state(g_pair_btn,
                         on ? LV_STATE_DEFAULT : LV_STATE_DISABLED,
                         !on);
    }
}

/* ------------------------------------------------------------------ */
/* Pairing button                                                       */
/* ------------------------------------------------------------------ */

static void pair_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

#if defined(CONFIG_BT)
    const struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS,
                      BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
        BT_DATA(BT_DATA_NAME_COMPLETE, "AkiraConsole",
                sizeof("AkiraConsole") - 1),
    };
    int ret = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
                               NULL, 0);
    if (ret < 0) {
        LOG_ERR("bt_le_adv_start failed: %d", ret);
    } else {
        LOG_INF("BT pairing advertising started");
        lv_label_set_text(lv_obj_get_child(g_pair_btn, 0),
                          "Advertising…");
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Back key                                                             */
/* ------------------------------------------------------------------ */

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
/* Screen construction                                                  */
/* ------------------------------------------------------------------ */

static void build_screen(void)
{
    g_screen = lv_obj_create(NULL);
    lv_obj_add_style(g_screen, &g_style_screen, 0);

    shell_theme_make_header(g_screen, "Bluetooth");
    shell_theme_make_footer(g_screen, "B:Back", "");

    int y = SHELL_HEADER_H + 12;

    /* Enable toggle */
    lv_obj_t *lbl_en = lv_label_create(g_screen);
    lv_label_set_text(lbl_en, "Bluetooth");
    lv_obj_add_style(lbl_en, &g_style_list_item, 0);
    lv_obj_align(lbl_en, LV_ALIGN_TOP_LEFT, 8, y);

    g_sw = lv_switch_create(g_screen);
    lv_obj_align(g_sw, LV_ALIGN_TOP_RIGHT, -8, y);
    lv_obj_add_event_cb(g_sw, sw_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

#if defined(CONFIG_BT)
    /* Assume BT is ON if already enabled */
    lv_obj_add_state(g_sw, LV_STATE_CHECKED);
#endif

    y += 36;

    /* Separator */
    lv_obj_t *sep = lv_obj_create(g_screen);
    lv_obj_set_size(sep, SHELL_SCREEN_W - 16, 1);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_style(sep, &g_style_separator, 0);
    y += 10;

    /* Pairing button */
    g_pair_btn = lv_btn_create(g_screen);
    lv_obj_set_size(g_pair_btn, 160, 36);
    lv_obj_align(g_pair_btn, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_event_cb(g_pair_btn, pair_click_cb, LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *pair_lbl = lv_label_create(g_pair_btn);
    lv_label_set_text(pair_lbl, "Enter Pairing Mode");
    lv_obj_center(pair_lbl);

    y += 44;

    /* Connected device label */
    g_conn_label = lv_label_create(g_screen);
    lv_label_set_text(g_conn_label, "Not connected");
    lv_obj_set_style_text_font(g_conn_label, SHELL_FONT_SMALL, 0);
    lv_obj_set_style_text_color(g_conn_label,
                                lv_color_make(0x60, 0x60, 0x60), 0);
    lv_obj_align(g_conn_label, LV_ALIGN_TOP_LEFT, 8, y);

    lv_obj_add_event_cb(g_screen, back_event_cb, LV_EVENT_KEY, NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void ble_screen_load(void)
{
    if (!g_screen) {
        build_screen();
    }
    lv_scr_load(g_screen);
}
