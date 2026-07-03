/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_ota_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ota_screen, CONFIG_AKIRA_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include "ota_screen.h"
#include "../settings_shared.h"

#if defined(CONFIG_AKIRA_OTA)
#include "../../connectivity/ota/ota_manager.h"
#endif

static void draw(void)
{
    akira_display_clear(SS_C_BLACK);
    ss_draw_header("FIRMWARE UPDATE");

    /* Version info panel */
    char ver[48];
    snprintf(ver, sizeof(ver), "Version: %s", CONFIG_AKIRA_OS_VERSION);

    int px = SS_MENU_X, py = SS_CONT_Y + 6;
    int pw = SS_MENU_W, ph = 26;

    akira_display_rounded_rect_fill(px, py, pw, ph, 4, SS_C_BLACK);
    akira_display_rounded_rect(px, py, pw, ph, 4, SS_C_WHITE);
    akira_display_text(px + 8, py + 8, ver, SS_C_WHITE);

    akira_display_text(px + 8, py + ph + 10,
#if defined(CONFIG_AKIRA_OTA)
        "OTA via web upload only",
#else
        "OTA not enabled in this build",
#endif
        SS_C_WHITE);

    ss_draw_ribbon("", "[B] Back");
    akira_display_flush();
}

void ota_screen_load(void)
{
    extern void settings_screen_load(void);
    draw();
    uint32_t prev = akira_input_get_bitmask();
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns = akira_input_get_bitmask(), just = btns & ~prev;
        prev = btns;
        if (!just) continue;
        if ((just & BIT(AKIRA_BTN_B)) || (just & BIT(AKIRA_BTN_HOME)) ||
            (just & BIT(AKIRA_BTN_A))) {
            settings_screen_load();
            return;
        }
    }
}
