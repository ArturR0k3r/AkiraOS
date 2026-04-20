/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_datetime_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_datetime_screen, CONFIG_AKIRA_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include "datetime_screen.h"
#include "../shell_theme.h"

#if defined(CONFIG_SNTP)
#include <zephyr/net/sntp.h>
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

static void draw(int sel) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t; gmtime_r(&ts.tv_sec, &t);
    char tbuf[32]; snprintf(tbuf,sizeof(tbuf),"%04d-%02d-%02d  %02d:%02d:%02d UTC",
        1900+t.tm_year,1+t.tm_mon,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
    akira_display_clear(C_BLACK); lc_header("DATE & TIME");
    /* clock display */
    akira_display_rounded_rect_fill(ITEM_X,LIST_Y+4,ITEM_W,26,4,C_BLACK);
    akira_display_rounded_rect(ITEM_X,LIST_Y+4,ITEM_W,26,4,C_DKGRAY);
    lc_centred(ITEM_X,LIST_Y+11,ITEM_W,tbuf,C_WHITE,C_BLACK);
    lc_item(0,sel,"Sync NTP","");
    lc_item(1,sel,"Back","");
    lc_footer("A-Select  |  B-Back");
    akira_display_flush();
}

void datetime_screen_load(void)
{
    extern void settings_screen_load(void);
    int sel=0; draw(sel);
    uint32_t prev=0;
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns=akira_input_get_bitmask(), just=btns&~prev; prev=btns;
        if (!just) continue;
        if (just&BIT(AKIRA_BTN_UP)) { if(sel>0){sel--;draw(sel);} }
        if (just&BIT(AKIRA_BTN_DOWN)) { if(sel<1){sel++;draw(sel);} }
        if (just&BIT(AKIRA_BTN_A)) {
            if (sel==0) {
#if defined(CONFIG_SNTP)
                struct sntp_time st; int r=sntp_simple("pool.ntp.org",5000,&st);
                if (!r) { struct timespec ts2={(time_t)st.seconds,0}; clock_settime(CLOCK_REALTIME,&ts2); }
                else LOG_ERR("SNTP:%d",r);
#endif
                draw(sel);
            } else {
                settings_screen_load(); return;
            }
        }
        if ((just&BIT(AKIRA_BTN_B))||(just&BIT(AKIRA_BTN_HOME))) { settings_screen_load(); return; }
    }
}
