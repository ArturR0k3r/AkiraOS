/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_power_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_power_screen, CONFIG_AKIRA_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <settings/settings.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include "power_screen.h"
#include "../settings_shared.h"

#define SLEEP_MIN   30
#define SLEEP_MAX   3600
#define SLEEP_STEP  30
#define DISP_MIN    5
#define DISP_MAX    600
#define DISP_STEP   5
#define NUM_ITEMS   2

static int g_sleep_s   = 300;
static int g_dispoff_s = 60;

static void draw(int sel)
{
    char sv[8], dv[8];
    snprintf(sv, sizeof(sv), "%ds", g_sleep_s);
    snprintf(dv, sizeof(dv), "%ds", g_dispoff_s);

    const char *labels[NUM_ITEMS]  = { "Sleep Timeout", "Display Off" };
    const char *rvalues[NUM_ITEMS] = { sv, dv };

    akira_display_clear(SS_C_BLACK);
    ss_draw_header("POWER");
    akira_display_rect(0, SS_CONT_Y, SS_SCR_W, SS_RIB_Y - SS_CONT_Y, SS_C_BLACK);

    for (int i = 0; i < NUM_ITEMS; i++) {
        int bx = SS_MENU_X;
        int by = SS_CONT_Y + i * SS_MENU_ITH + 2;
        int bw = SS_MENU_W;
        int bh = SS_MENU_ITH - 4;
        bool hi = (i == sel);

        if (hi) {
            ss_glass_rect_focus(bx, by, bw, bh, 5);
        } else {
            ss_glass_rect_dim(bx, by, bw, bh, 5);
        }
        int ty = by + (bh - 10) / 2;
        uint16_t fg = hi ? SS_C_BLACK : SS_C_WHITE;
        akira_display_text(bx + 10, ty, labels[i], fg);
        int rvlen = (int)strlen(rvalues[i]);
        akira_display_text(bx + bw - rvlen * 8 - 10, ty, rvalues[i], fg);
    }

    ss_draw_ribbon("[</> Adjust", "[B] Back");
    akira_display_flush();
}

void power_screen_load(void)
{
    extern void settings_screen_load(void);
    { char _sv[16] = ""; if (!akira_settings_get("akira/power/sleep_s",   _sv, sizeof(_sv))) g_sleep_s   = atoi(_sv); }
    { char _sv[16] = ""; if (!akira_settings_get("akira/power/dispoff_s", _sv, sizeof(_sv))) g_dispoff_s = atoi(_sv); }

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

        if (just & BIT(AKIRA_BTN_LEFT)) {
            if (sel == 0) {
                g_sleep_s -= SLEEP_STEP;
                if (g_sleep_s < SLEEP_MIN) g_sleep_s = SLEEP_MIN;
                { char _sv[16]; snprintf(_sv, sizeof(_sv), "%d", g_sleep_s); akira_settings_set("akira/power/sleep_s", _sv, 0); }
            } else {
                g_dispoff_s -= DISP_STEP;
                if (g_dispoff_s < DISP_MIN) g_dispoff_s = DISP_MIN;
                { char _sv[16]; snprintf(_sv, sizeof(_sv), "%d", g_dispoff_s); akira_settings_set("akira/power/dispoff_s", _sv, 0); }
            }
            draw(sel);
        }
        if (just & BIT(AKIRA_BTN_RIGHT)) {
            if (sel == 0) {
                g_sleep_s += SLEEP_STEP;
                if (g_sleep_s > SLEEP_MAX) g_sleep_s = SLEEP_MAX;
                { char _sv[16]; snprintf(_sv, sizeof(_sv), "%d", g_sleep_s); akira_settings_set("akira/power/sleep_s", _sv, 0); }
            } else {
                g_dispoff_s += DISP_STEP;
                if (g_dispoff_s > DISP_MAX) g_dispoff_s = DISP_MAX;
                { char _sv[16]; snprintf(_sv, sizeof(_sv), "%d", g_dispoff_s); akira_settings_set("akira/power/dispoff_s", _sv, 0); }
            }
            draw(sel);
        }
        if ((just & BIT(AKIRA_BTN_B)) || (just & BIT(AKIRA_BTN_HOME))) {
            settings_screen_load();
            return;
        }
    }
}
