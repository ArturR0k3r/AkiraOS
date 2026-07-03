/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_devmode_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_devmode_screen, CONFIG_AKIRA_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <settings/settings.h>
#include <zephyr/version.h>
#include <string.h>
#include <stdio.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include "devmode_screen.h"
#include "../settings_shared.h"

static bool g_devmode_enabled;

static void draw(int sel)
{
    akira_display_clear(SS_C_BLACK);
    ss_draw_header("DEVELOPER MODE");
    akira_display_rect(0, SS_CONT_Y, SS_SCR_W, SS_RIB_Y - SS_CONT_Y, SS_C_BLACK);

    /* Single toggle item */
    {
        int bx = SS_MENU_X;
        int by = SS_CONT_Y + 2;
        int bw = SS_MENU_W;
        int bh = SS_MENU_ITH - 4;
        bool hi = (sel == 0);
        const char *rv = g_devmode_enabled ? "ON" : "OFF";

        if (hi) {
            ss_glass_rect_focus(bx, by, bw, bh, 5);
        } else {
            ss_glass_rect_dim(bx, by, bw, bh, 5);
        }
        int ty = by + (bh - 10) / 2;
        uint16_t fg = hi ? SS_C_BLACK : SS_C_WHITE;
        akira_display_text(bx + 10, ty, "Dev Mode", fg);
        int rvlen = (int)strlen(rv);
        akira_display_text(bx + bw - rvlen * 8 - 10, ty, rv, fg);
    }

    /* Info panel below toggle */
    int iy = SS_CONT_Y + SS_MENU_ITH + 6;
    if (g_devmode_enabled) {
        char buf[80];
        snprintf(buf, sizeof(buf), "OS: %s  Zephyr: %d.%d.%d",
                 CONFIG_AKIRA_OS_VERSION,
                 (int)KERNEL_VERSION_MAJOR,
                 (int)KERNEL_VERSION_MINOR,
                 (int)KERNEL_PATCHLEVEL);
        akira_display_text(SS_MENU_X + 4, iy, buf, SS_C_GRAY);
    } else {
        akira_display_text(SS_MENU_X + 4, iy, "Enable to see debug info", SS_C_DKGRAY);
    }

    ss_draw_ribbon("[A] Toggle", "[B] Back");
    akira_display_flush();
}

void devmode_screen_load(void)
{
    extern void settings_screen_load(void);
    { char _sv[4] = ""; akira_settings_get("akira/devmode/enabled", _sv, sizeof(_sv)); g_devmode_enabled = (_sv[0] == '1'); }
    int sel = 0;
    draw(sel);
    uint32_t prev = akira_input_get_bitmask();
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns = akira_input_get_bitmask(), just = btns & ~prev;
        prev = btns;
        if (!just) continue;
        if (just & BIT(AKIRA_BTN_A)) {
            g_devmode_enabled = !g_devmode_enabled;
            akira_settings_set("akira/devmode/enabled", g_devmode_enabled ? "1" : "0", 0);
            draw(sel);
        }
        if ((just & BIT(AKIRA_BTN_B)) || (just & BIT(AKIRA_BTN_HOME))) {
            settings_screen_load();
            return;
        }
    }
}
