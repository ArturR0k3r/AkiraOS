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

#define MAX_APPS CONFIG_AKIRA_APP_MAX_INSTALLED

static app_info_t g_apps[MAX_APPS];
static int g_count;
static int g_sel, g_scroll;

static int vis_count(void) { return (FOOT_Y - LIST_Y) / ITEM_H; }

static void draw(void) {
    akira_display_clear(C_BLACK);
    lc_header("APPS");
    int vis = vis_count();
    if (g_scroll > g_count - vis) g_scroll = g_count - vis;
    if (g_scroll < 0) g_scroll = 0;
    if (g_count == 0) {
        lc_centred(0,LIST_Y+40,SCR_W,"No apps installed",C_GRAY,C_BLACK);
    } else {
        for (int i = g_scroll; i < g_count && i < g_scroll+vis; i++) {
            char sz[16]; snprintf(sz,sizeof(sz),"%uKB",(unsigned)((g_apps[i].size+1023)/1024));
            /* Draw the item but with name limited to 24 chars */
            char nm[25]; strncpy(nm,g_apps[i].name,24); nm[24]=0;
            lc_item(i-g_scroll, g_sel-g_scroll, nm, sz);
        }
    }
    lc_footer("A-Uninstall  |  B-Back");
    akira_display_flush();
}

static void draw_confirm(const char *name) {
    int pw=240,ph=70; int px=(SCR_W-pw)/2, py=(SCR_H-ph)/2;
    for (int qy=py;qy<py+ph;qy++) for (int qx=px;qx<px+pw;qx++) if ((qx^qy)&1) akira_display_pixel(qx,qy,C_GLASS);
    akira_display_rect(px,py,pw,ph,C_BLACK);
    akira_display_rect_outline(px,py,pw,ph,C_WHITE); akira_display_rect_outline(px+1,py+1,pw-2,ph-2,C_WHITE);
    char msg[48]; snprintf(msg,sizeof(msg),"Uninstall %s ?",name);
    lc_centred(px+4,py+8,pw-8,msg,C_WHITE,C_BLACK);
    akira_display_hline(px+4,py+22,pw-8,C_WHITE);
    lc_item(0,0,"YES","");
    lc_item(1,-1,"CANCEL","");
    akira_display_flush();
}

void apps_screen_load(void)
{
    extern void settings_screen_load(void);
    g_count = app_manager_list(g_apps, MAX_APPS);
    if (g_count < 0) g_count = 0;
    g_sel = 0; g_scroll = 0;
    draw();
    uint32_t prev = 0;
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns = akira_input_get_bitmask(), just = btns&~prev; prev=btns;
        if (!just) continue;
        int vis = vis_count();
        if (just&BIT(AKIRA_BTN_UP)) { if(g_sel>0){g_sel--;if(g_sel<g_scroll)g_scroll--;draw();} }
        if (just&BIT(AKIRA_BTN_DOWN)) { if(g_sel<g_count-1){g_sel++;if(g_sel>=g_scroll+vis)g_scroll++;draw();} }
        if ((just&BIT(AKIRA_BTN_A)) && g_count>0) {
            draw_confirm(g_apps[g_sel].name);
            /* Wait for confirm */
            uint32_t cp=0; int cs=0;
            while (true) {
                k_sleep(K_MSEC(20));
                uint32_t cb=akira_input_get_bitmask(), cj=cb&~cp; cp=cb; if(!cj) continue;
                if (cj&BIT(AKIRA_BTN_UP)) { if(cs>0){cs--;draw_confirm(g_apps[g_sel].name);} }
                if (cj&BIT(AKIRA_BTN_DOWN)) { if(cs<1){cs++;draw_confirm(g_apps[g_sel].name);} }
                if (cj&BIT(AKIRA_BTN_A)) {
                    if (cs==0) {
                        app_manager_uninstall(g_apps[g_sel].name);
                        g_count = app_manager_list(g_apps, MAX_APPS);
                        if (g_count<0) g_count=0;
                        if (g_sel>=g_count) g_sel=g_count>0?g_count-1:0;
                        g_scroll=0;
                    }
                    break;
                }
                if ((cj&BIT(AKIRA_BTN_B))||(cj&BIT(AKIRA_BTN_HOME))) break;
            }
            draw();
        }
        if ((just&BIT(AKIRA_BTN_B))||(just&BIT(AKIRA_BTN_HOME))) { settings_screen_load(); return; }
    }
}
