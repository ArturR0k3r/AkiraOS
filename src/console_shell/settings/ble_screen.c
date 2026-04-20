/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_ble_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ble_screen, CONFIG_AKIRA_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>

#include "shell_theme.h"
#include <connectivity/bluetooth/bt_manager.h>
#include "ble_screen.h"



static void centred(int x, int y, int w, const char *s, uint16_t fg, uint16_t bg)
{
    int tw = (int)(strlen(s) * 8);
    int lx = x + (tw < w ? (w - tw) / 2 : 0);
    akira_display_rect(x, y, w, 10, bg);
    akira_display_text(lx, y, s, fg);
}

static void draw_item(int idx, int sel, const char *lbl, const char *rv)
{
    int iy = LIST_Y + idx * ITEM_H;
    bool hi = (idx == sel);
    uint16_t ibg = hi ? C_WHITE : C_BLACK;
    uint16_t ifg = hi ? C_BLACK : C_WHITE;

    akira_display_rounded_rect_fill(ITEM_X, iy + 3, ITEM_W, ITEM_H - 6, 5, ibg);
    akira_display_rounded_rect(ITEM_X, iy + 3, ITEM_W, ITEM_H - 6, 5,
                               hi ? C_BLACK : C_DKGRAY);
    int ty = iy + 3 + (ITEM_H - 6 - 10) / 2;
    akira_display_text(ITEM_X + 10, ty, lbl, ifg);
    if (rv && rv[0]) {
        int tw = (int)strlen(rv) * 8;
        akira_display_text(ITEM_X + ITEM_W - tw - 10, ty, rv, ifg);
    }
}

static const char *state_label(bt_state_t st)
{
    switch (st) {
    case BT_STATE_OFF:          return "OFF";
    case BT_STATE_INITIALIZING: return "Init...";
    case BT_STATE_READY:        return "Ready";
    case BT_STATE_ADVERTISING:  return "Advertising";
    case BT_STATE_CONNECTED:    return "Connected";
    case BT_STATE_PAIRING:      return "Pairing";
    case BT_STATE_ERROR:        return "Error";
    default:                    return "Unknown";
    }
}

static void draw(int sel)
{
    bt_state_t st = bt_manager_get_state();
    bool on       = (st != BT_STATE_OFF && st != BT_STATE_ERROR);

    akira_display_clear(C_BLACK);
    akira_display_rect(0, 0, SCR_W, SBAR_H, C_BLACK);
    centred(0, (SBAR_H - 10) / 2, SCR_W, "BLUETOOTH", C_WHITE, C_BLACK);
    akira_display_hline(0, SBAR_H,     SCR_W, C_WHITE);
    akira_display_hline(0, SBAR_H + 1, SCR_W, C_WHITE);

    draw_item(0, sel, "Bluetooth",    on ? "ON" : "OFF");
    draw_item(1, sel, "Pairing Mode", state_label(st));

    /* Connection info */
    char addr[48] = "";
    bt_manager_get_address(addr, sizeof(addr));
    int info_y = LIST_Y + 2 * ITEM_H + 10;
    if (bt_manager_is_connected()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Connected: %s", addr);
        akira_display_text(ITEM_X + 6, info_y, buf, C_GRAY);
    } else {
        akira_display_text(ITEM_X + 6, info_y, "Not connected", C_DKGRAY);
    }

    akira_display_hline(0, FOOT_Y - 1, SCR_W, C_WHITE);
    akira_display_hline(0, FOOT_Y - 2, SCR_W, C_WHITE);
    akira_display_rect(0, FOOT_Y, SCR_W, FOOT_H, C_BLACK);
    akira_display_text(8, FOOT_Y + 7, "A-Toggle  |  B-Back", C_WHITE);
    akira_display_flush();
}

void ble_screen_load(void)
{
    extern void settings_screen_load(void);

    int sel = 0;
    draw(sel);

    uint32_t prev = 0;
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns = akira_input_get_bitmask();
        uint32_t just = btns & ~prev;
        prev = btns;
        if (!just) continue;

        if (just & BIT(AKIRA_BTN_UP)) {
            if (sel > 0) { sel--; draw(sel); }
        }
        if (just & BIT(AKIRA_BTN_DOWN)) {
            if (sel < 1) { sel++; draw(sel); }
        }
        if (just & BIT(AKIRA_BTN_A)) {
            bt_state_t st = bt_manager_get_state();
            if (sel == 0) {
                if (st == BT_STATE_OFF || st == BT_STATE_ERROR) {
                    int r = bt_manager_init(NULL);
                    if (r < 0) LOG_ERR("bt_manager_init: %d", r);
                } else {
                    int r = bt_manager_deinit();
                    if (r < 0) LOG_ERR("bt_manager_deinit: %d", r);
                }
            } else if (sel == 1) {
                if (st == BT_STATE_ADVERTISING) {
                    int r = bt_manager_stop_advertising();
                    if (r < 0) LOG_ERR("bt_manager_stop_advertising: %d", r);
                } else if (st == BT_STATE_READY || st == BT_STATE_CONNECTED) {
                    int r = bt_manager_start_advertising();
                    if (r < 0) LOG_ERR("bt_manager_start_advertising: %d", r);
                }
            }
            draw(sel);
        }
        if ((just & BIT(AKIRA_BTN_B)) || (just & BIT(AKIRA_BTN_HOME))) {
            settings_screen_load();
            return;
        }
    }
}
