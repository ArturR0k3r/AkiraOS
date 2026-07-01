/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_apps_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_apps_screen, CONFIG_AKIRA_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <runtime/app_manager/app_manager.h>
#include "apps_screen.h"
#include "../settings_shared.h"
#include "ui/akira_ui.h"

#define MAX_APPS CONFIG_AKIRA_APP_MAX_INSTALLED

static app_info_t g_apps[MAX_APPS] __attribute__((section(".ext_ram.bss")));
static int g_count;
static int g_sel, g_scroll;

/* Draw one app item at visual row `row` with focus flag `hi`. */
static void draw_app_item(int row, bool hi, const char *name, const char *sz)
{
    int bx = SS_MENU_X;
    int by = SS_CONT_Y + row * SS_MENU_ITH + 2;
    int bw = SS_MENU_W;
    int bh = SS_MENU_ITH - 4;

    if (hi) {
        ss_glass_rect_focus(bx, by, bw, bh, 5);
    } else {
        ss_glass_rect_dim(bx, by, bw, bh, 5);
    }
    int ty = by + (bh - 10) / 2;
    uint16_t fg = hi ? SS_C_WHITE : SS_C_DKGRAY;
    akira_display_text(bx + 10, ty, name, fg);
    if (sz && sz[0]) {
        int szlen = (int)strlen(sz);
        akira_display_text(bx + bw - szlen * 8 - 10, ty, sz, fg);
    }
}

static void draw(void)
{
    akira_display_clear(SS_C_BLACK);
    ss_draw_header("APPS");

    int vis = ss_menu_vis_count(SS_CONT_Y);
    if (g_scroll > g_count - vis) g_scroll = g_count - vis;
    if (g_scroll < 0)             g_scroll = 0;

    akira_display_rect(0, SS_CONT_Y, SS_SCR_W, SS_RIB_Y - SS_CONT_Y, SS_C_BLACK);

    if (g_count == 0) {
        /* Centred placeholder */
        int ty = SS_CONT_Y + (SS_RIB_Y - SS_CONT_Y - 10) / 2;
        int msg_len = (int)strlen("No apps installed") * 8;
        akira_display_text((SS_SCR_W - msg_len) / 2, ty, "No apps installed", SS_C_GRAY);
    } else {
        for (int i = g_scroll; i < g_count && i < g_scroll + vis; i++) {
            char sz[16];
            snprintf(sz, sizeof(sz), "%uKB",
                     (unsigned)((g_apps[i].size + 1023) / 1024));
            char nm[25];
            strncpy(nm, g_apps[i].name, 24);
            nm[24] = '\0';
            draw_app_item(i - g_scroll, (i == g_sel), nm, sz);
        }
        /* Scroll indicators */
        if (g_scroll > 0) {
            akira_display_text(SS_SCR_W - 10, SS_CONT_Y + 2, "^", SS_C_GRAY);
        }
        if (g_scroll + vis < g_count) {
            int bot_y = SS_CONT_Y + vis * SS_MENU_ITH + 2;
            if (bot_y < SS_RIB_Y - 10) {
                akira_display_text(SS_SCR_W - 10, bot_y, "v", SS_C_GRAY);
            }
        }
    }

    ss_draw_ribbon("[A] Uninstall", "[B] Back");
    akira_display_flush();
}

void apps_screen_load(void)
{
    extern void settings_screen_load(void);
    g_count = app_manager_list(g_apps, MAX_APPS);
    if (g_count < 0) g_count = 0;
    g_sel = 0; g_scroll = 0;
    draw();
    uint32_t prev = akira_input_get_bitmask();
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns = akira_input_get_bitmask(), just = btns & ~prev;
        prev = btns;
        if (!just) continue;

        int vis = ss_menu_vis_count(SS_CONT_Y);

        if (just & BIT(AKIRA_BTN_UP)) {
            if (g_sel > 0) {
                g_sel--;
                g_scroll = ss_scroll_clamp(g_sel, g_scroll, g_count, SS_CONT_Y);
                draw();
            }
        }
        if (just & BIT(AKIRA_BTN_DOWN)) {
            if (g_sel < g_count - 1) {
                g_sel++;
                g_scroll = ss_scroll_clamp(g_sel, g_scroll, g_count, SS_CONT_Y);
                draw();
            }
        }

        if ((just & BIT(AKIRA_BTN_A)) && g_count > 0) {
            /* Uninstalling an installed app is a guarded action — route it
             * through the shared 3px Capability-Guard confirmation. */
            char q[48];
            snprintf(q, sizeof(q), "Uninstall %s?", g_apps[g_sel].name);
            if (akira_ui_confirm_dialog("APP_UNINSTALL", q)) {
                app_manager_uninstall(g_apps[g_sel].name);
                g_count = app_manager_list(g_apps, MAX_APPS);
                if (g_count < 0) g_count = 0;
                if (g_sel >= g_count) g_sel = g_count > 0 ? g_count - 1 : 0;
                g_scroll = 0;
            }
            prev = akira_input_get_bitmask();  /* swallow buttons held from dialog */
            draw();
        }

        if ((just & BIT(AKIRA_BTN_B)) || (just & BIT(AKIRA_BTN_HOME))) {
            settings_screen_load();
            return;
        }

        (void)vis;
    }
}
