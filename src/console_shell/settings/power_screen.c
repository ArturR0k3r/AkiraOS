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
#include "../shell_theme.h"

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

#define SLEEP_MIN   30
#define SLEEP_MAX   3600
#define SLEEP_STEP  30
#define DISP_MIN    5
#define DISP_MAX    600
#define DISP_STEP   5

static int g_sleep_s   = 300;
static int g_dispoff_s = 60;

static void draw(int sel) {
    akira_display_clear(C_BLACK); lc_header("POWER");
    char sv[8]; snprintf(sv,sizeof(sv),"%ds",g_sleep_s);
    char dv[8]; snprintf(dv,sizeof(dv),"%ds",g_dispoff_s);
    lc_item(0,sel,"Sleep Timeout",sv);
    lc_item(1,sel,"Display Off",dv);
    lc_footer("LR-Adjust  |  B-Back");
    akira_display_flush();
}

void power_screen_load(void)
{
    extern void settings_screen_load(void);
    { char _sv[16]=""; if(!akira_settings_get("akira/power/sleep_s",_sv,sizeof(_sv))) g_sleep_s=atoi(_sv); }
    { char _sv[16]=""; if(!akira_settings_get("akira/power/dispoff_s",_sv,sizeof(_sv))) g_dispoff_s=atoi(_sv); }
    int sel=0; draw(sel);
    uint32_t prev=0;
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns=akira_input_get_bitmask(), just=btns&~prev; prev=btns;
        if (!just) continue;
        if (just&BIT(AKIRA_BTN_UP)) { if(sel>0){sel--;draw(sel);} }
        if (just&BIT(AKIRA_BTN_DOWN)) { if(sel<1){sel++;draw(sel);} }
        if (just&BIT(AKIRA_BTN_LEFT)) {
            if (sel==0){g_sleep_s-=SLEEP_STEP;if(g_sleep_s<SLEEP_MIN)g_sleep_s=SLEEP_MIN;{char _sv[16];snprintf(_sv,sizeof(_sv),"%d",g_sleep_s);akira_settings_set("akira/power/sleep_s",_sv,0);}}
            else {g_dispoff_s-=DISP_STEP;if(g_dispoff_s<DISP_MIN)g_dispoff_s=DISP_MIN;{char _sv[16];snprintf(_sv,sizeof(_sv),"%d",g_dispoff_s);akira_settings_set("akira/power/dispoff_s",_sv,0);}}
            draw(sel);
        }
        if (just&BIT(AKIRA_BTN_RIGHT)) {
            if (sel==0){g_sleep_s+=SLEEP_STEP;if(g_sleep_s>SLEEP_MAX)g_sleep_s=SLEEP_MAX;{char _sv[16];snprintf(_sv,sizeof(_sv),"%d",g_sleep_s);akira_settings_set("akira/power/sleep_s",_sv,0);}}
            else {g_dispoff_s+=DISP_STEP;if(g_dispoff_s>DISP_MAX)g_dispoff_s=DISP_MAX;{char _sv[16];snprintf(_sv,sizeof(_sv),"%d",g_dispoff_s);akira_settings_set("akira/power/dispoff_s",_sv,0);}}
            draw(sel);
        }
        if ((just&BIT(AKIRA_BTN_B))||(just&BIT(AKIRA_BTN_HOME))) { settings_screen_load(); return; }
    }
}
