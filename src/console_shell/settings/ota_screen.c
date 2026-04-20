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
#include "../shell_theme.h"

#if defined(CONFIG_AKIRA_OTA)
#include "../../connectivity/ota/ota_manager.h"
#endif

static void lc_centred(int x, int y, int w, const char *s, uint16_t fg, uint16_t bg) {
    int tw = (int)(strlen(s)*8); int lx = x+(tw<w?(w-tw)/2:0);
    akira_display_rect(x,y,w,10,bg); akira_display_text(lx,y,s,fg);
}
static void lc_header(const char *title) {
    akira_display_rect(0,0,SCR_W,SBAR_H,C_BLACK);
    lc_centred(0,(SBAR_H-10)/2,SCR_W,title,C_WHITE,C_BLACK);
    akira_display_hline(0,SBAR_H,SCR_W,C_WHITE); akira_display_hline(0,SBAR_H+1,SCR_W,C_WHITE);
}
static void lc_footer(const char *txt) {
    akira_display_hline(0,FOOT_Y-1,SCR_W,C_WHITE); akira_display_hline(0,FOOT_Y-2,SCR_W,C_WHITE);
    akira_display_rect(0,FOOT_Y,SCR_W,FOOT_H,C_BLACK); akira_display_text(8,FOOT_Y+7,txt,C_WHITE);
}
static void lc_item(int idx, int sel, const char *lbl, const char *rv) {
    int iy=LIST_Y+idx*ITEM_H; bool hi=(idx==sel);
    uint16_t ibg=hi?C_WHITE:C_BLACK, ifg=hi?C_BLACK:C_WHITE;
    akira_display_rounded_rect_fill(ITEM_X,iy+3,ITEM_W,ITEM_H-6,5,ibg);
    akira_display_rounded_rect(ITEM_X,iy+3,ITEM_W,ITEM_H-6,5,hi?C_BLACK:C_DKGRAY);
    int ty=iy+3+(ITEM_H-6-10)/2;
    akira_display_text(ITEM_X+10,ty,lbl,ifg);
    if (rv&&rv[0]){int tw=(int)strlen(rv)*8; akira_display_text(ITEM_X+ITEM_W-tw-10,ty,rv,ifg);}
}
static void lc_bar(int x, int y, int w, int h, int pct) {
    akira_display_rect_outline(x-1,y-1,w+2,h+2,C_WHITE);
    int fw = pct*w/100;
    akira_display_rect(x,y,w,h,C_DKGRAY);
    if (fw>0) akira_display_rect(x,y,fw,h,C_WHITE);
}

static void draw(void) {
    akira_display_clear(C_BLACK); lc_header("FIRMWARE UPDATE");
    char ver[48]; snprintf(ver,sizeof(ver),"Version: %s",CONFIG_AKIRA_OS_VERSION);
    akira_display_rounded_rect_fill(ITEM_X,LIST_Y+4,ITEM_W,22,4,C_BLACK);
    akira_display_rounded_rect(ITEM_X,LIST_Y+4,ITEM_W,22,4,C_DKGRAY);
    akira_display_text(ITEM_X+8,LIST_Y+11,ver,C_WHITE);
    akira_display_text(ITEM_X+8,LIST_Y+32,
#if defined(CONFIG_AKIRA_OTA)
        "OTA via web upload only",
#else
        "OTA not enabled in this build",
#endif
        C_GRAY);
    lc_footer("B-Back");
    akira_display_flush();
}

void ota_screen_load(void)
{
    extern void settings_screen_load(void);
    draw();
    uint32_t prev=0;
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns=akira_input_get_bitmask(), just=btns&~prev; prev=btns;
        if (!just) continue;
        if ((just&BIT(AKIRA_BTN_B))||(just&BIT(AKIRA_BTN_HOME))||(just&BIT(AKIRA_BTN_A))) { settings_screen_load(); return; }
    }
}
