/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_display_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_display_screen, CONFIG_AKIRA_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <settings/settings.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <drivers/platform_hal.h>
#include "display_screen.h"
#include "../settings_shared.h"

#define BRIGHT_MIN 10
#define BRIGHT_MAX 100
#define BRIGHT_STEP 10
#define TMOUT_MIN 5
#define TMOUT_MAX 300
#define TMOUT_STEP 5

typedef enum
{
    ITEM_BRIGHTNESS = 0,
    ITEM_TIMEOUT_EN,
    ITEM_TIMEOUT_S,
    NUM_ITEMS,
} disp_item_t;

static int g_brightness = BRIGHT_MAX;
static int g_timeout = 180;
static bool g_timeout_en = true;

static void apply_brightness(int v)
{
    uint8_t hw = (uint8_t)((v * 255 + 50) / 100);
    akira_display_hal_set_brightness(hw);
    LOG_DBG("Bright=%d -> hw=%u", v, hw);
}

static void draw(int sel)
{
    char bv[8], tv[8];
    snprintf(bv, sizeof(bv), "%d", g_brightness);
    snprintf(tv, sizeof(tv), "%ds", g_timeout);

    const char *labels[NUM_ITEMS] = {"Brightness", "Auto Sleep", "Screen Off (s)"};
    const char *rvalues[NUM_ITEMS] = {bv, g_timeout_en ? "ON" : "OFF", tv};

    akira_display_clear(SS_C_BLACK);
    ss_draw_header("DISPLAY");
    akira_display_rect(0, SS_CONT_Y, SS_SCR_W, SS_RIB_Y - SS_CONT_Y, SS_C_BLACK);

    for (int i = 0; i < NUM_ITEMS; i++)
    {
        int bx = SS_MENU_X;
        int by = SS_CONT_Y + i * SS_MENU_ITH + 2;
        int bw = SS_MENU_W;
        int bh = SS_MENU_ITH - 4;
        bool hi = (i == sel);
        /* Duration row is dimmed while auto sleep is disabled */
        bool inactive = (i == ITEM_TIMEOUT_S && !g_timeout_en);

        if (hi && !inactive)
        {
            ss_glass_rect_focus(bx, by, bw, bh, 5);
        }
        else
        {
            ss_glass_rect_dim(bx, by, bw, bh, 5);
        }
        int ty = by + (bh - 10) / 2;
        uint16_t fg = (hi && !inactive) ? SS_C_WHITE : SS_C_DKGRAY;
        akira_display_text(bx + 10, ty, labels[i], fg);
        int rvlen = (int)strlen(rvalues[i]);
        akira_display_text(bx + bw - rvlen * 8 - 10, ty, rvalues[i], fg);
    }

    ss_draw_ribbon("[</> Adjust", "[B] Back");
    akira_display_flush();
}

void display_screen_load(void)
{
    extern void settings_screen_load(void);
    {
        char _sv[16] = "";
        if (!akira_settings_get("akira/display/brightness", _sv, sizeof(_sv)))
        {
            g_brightness = atoi(_sv);
            if (g_brightness > BRIGHT_MAX)
                g_brightness = BRIGHT_MAX;
            if (g_brightness < BRIGHT_MIN)
                g_brightness = BRIGHT_MIN;
        }
    }
    {
        char _sv[16] = "";
        if (!akira_settings_get("akira/display/timeout_s", _sv, sizeof(_sv)))
            g_timeout = atoi(_sv);
    }
    {
        char _sv[4] = "";
        if (!akira_settings_get("akira/display/timeout_en", _sv, sizeof(_sv)))
            g_timeout_en = (atoi(_sv) != 0);
    }

    int sel = 0;
    draw(sel);
    uint32_t prev = akira_input_get_bitmask();
    while (true)
    {
        k_sleep(K_MSEC(20));
        uint32_t btns = akira_input_get_bitmask(), just = btns & ~prev;
        prev = btns;
        if (!just)
            continue;

        if (just & BIT(AKIRA_BTN_UP))
        {
            if (sel > 0)
            {
                sel--;
                draw(sel);
            }
        }
        if (just & BIT(AKIRA_BTN_DOWN))
        {
            if (sel < NUM_ITEMS - 1)
            {
                sel++;
                draw(sel);
            }
        }

        if (just & BIT(AKIRA_BTN_LEFT))
        {
            if (sel == ITEM_BRIGHTNESS)
            {
                g_brightness -= BRIGHT_STEP;
                if (g_brightness < BRIGHT_MIN)
                    g_brightness = BRIGHT_MIN;
                apply_brightness(g_brightness);
                {
                    char _sv[16];
                    snprintf(_sv, sizeof(_sv), "%d", g_brightness);
                    akira_settings_set("akira/display/brightness", _sv, 0);
                }
            }
            else if (sel == ITEM_TIMEOUT_EN)
            {
                g_timeout_en = false;
                akira_settings_set("akira/display/timeout_en", "0", 0);
            }
            else if (sel == ITEM_TIMEOUT_S && g_timeout_en)
            {
                g_timeout -= TMOUT_STEP;
                if (g_timeout < TMOUT_MIN)
                    g_timeout = TMOUT_MIN;
                {
                    char _sv[16];
                    snprintf(_sv, sizeof(_sv), "%d", g_timeout);
                    akira_settings_set("akira/display/timeout_s", _sv, 0);
                }
            }
            draw(sel);
        }
        if (just & BIT(AKIRA_BTN_RIGHT))
        {
            if (sel == ITEM_BRIGHTNESS)
            {
                g_brightness += BRIGHT_STEP;
                if (g_brightness > BRIGHT_MAX)
                    g_brightness = BRIGHT_MAX;
                apply_brightness(g_brightness);
                {
                    char _sv[16];
                    snprintf(_sv, sizeof(_sv), "%d", g_brightness);
                    akira_settings_set("akira/display/brightness", _sv, 0);
                }
            }
            else if (sel == ITEM_TIMEOUT_EN)
            {
                g_timeout_en = true;
                akira_settings_set("akira/display/timeout_en", "1", 0);
            }
            else if (sel == ITEM_TIMEOUT_S && g_timeout_en)
            {
                g_timeout += TMOUT_STEP;
                if (g_timeout > TMOUT_MAX)
                    g_timeout = TMOUT_MAX;
                {
                    char _sv[16];
                    snprintf(_sv, sizeof(_sv), "%d", g_timeout);
                    akira_settings_set("akira/display/timeout_s", _sv, 0);
                }
            }
            draw(sel);
        }
        if ((just & BIT(AKIRA_BTN_B)) || (just & BIT(AKIRA_BTN_HOME)))
        {
            settings_screen_load();
            return;
        }
    }
}
