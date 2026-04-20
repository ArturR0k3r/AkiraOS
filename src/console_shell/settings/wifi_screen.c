/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_wifi_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_wifi_screen, CONFIG_AKIRA_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <settings/settings.h>
#include <string.h>
#include <stdio.h>

#include "shell_theme.h"
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include "wifi_screen.h"

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#define WIFI_MAX_NETWORKS 16
static struct wifi_scan_result g_scan[WIFI_MAX_NETWORKS];
static int g_scan_count;
static struct k_sem g_scan_done;
static void on_wifi_event(struct net_mgmt_event_callback *cb, uint32_t ev, struct net_if *iface)
{
    if (ev==NET_EVENT_WIFI_SCAN_RESULT && g_scan_count<WIFI_MAX_NETWORKS)
        g_scan[g_scan_count++]=*(const struct wifi_scan_result*)cb->info;
    else if (ev==NET_EVENT_WIFI_SCAN_DONE)
        k_sem_give(&g_scan_done);
}
static struct net_mgmt_event_callback g_wifi_cb;
static bool g_wifi_cb_reg;
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

#define SSID_MAX 33
#define PASS_MAX 64

static char g_ssid_list[16][SSID_MAX];
static int  g_ssid_count;
static int  g_sel, g_scroll;

/* ---- Character spinner for password entry ---- */
static const char CHARSET[] =
    " abcdefghijklmnopqrstuvwxyz0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%&*()-_+=.,;:/?";
#define CHARSET_LEN ((int)(sizeof(CHARSET)-1))

static void draw_passentry(const char *ssid, const char *buf, int cur, int csel) {
    int pw=300,ph=96; int px=(SCR_W-pw)/2, py=(SCR_H-ph)/2;
    akira_display_rect(px,py,pw,ph,C_BLACK);
    akira_display_rect_outline(px,py,pw,ph,C_WHITE);
    akira_display_rect_outline(px+1,py+1,pw-2,ph-2,C_WHITE);
    char hdr[48]; snprintf(hdr,sizeof(hdr),"Password: %s",ssid);
    lc_centred(px+4,py+6,pw-8,hdr,C_WHITE,C_BLACK);
    akira_display_hline(px+4,py+18,pw-8,C_WHITE);
    /* Password so far + cursor */
    char disp[PASS_MAX+2]; memcpy(disp,buf,cur); disp[cur]=CHARSET[csel]; disp[cur+1]=0;
    akira_display_text(px+8,py+24,disp,C_WHITE);
    /* Hint */
    akira_display_text(px+8,py+40,"UP/DN:char  A:add  B:del  X:done",C_GRAY);
    /* Current char large */
    char cc[2]={CHARSET[csel],0};
    akira_display_text_large(px+pw/2-6,py+56,cc,C_WHITE);
    akira_display_flush();
}

static bool enter_password(const char *ssid, char *out, int maxlen) {
    char buf[PASS_MAX]; memset(buf,0,sizeof(buf));
    int cur=0, csel=0;
    draw_passentry(ssid,buf,cur,csel);
    uint32_t prev=0;
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns=akira_input_get_bitmask(), just=btns&~prev; prev=btns;
        if (!just) continue;
        if (just&BIT(AKIRA_BTN_UP)) { csel=(csel-1+CHARSET_LEN)%CHARSET_LEN; draw_passentry(ssid,buf,cur,csel); }
        if (just&BIT(AKIRA_BTN_DOWN)) { csel=(csel+1)%CHARSET_LEN; draw_passentry(ssid,buf,cur,csel); }
        if (just&BIT(AKIRA_BTN_A)) {
            if (cur<maxlen-1) { buf[cur++]=CHARSET[csel]; }
            draw_passentry(ssid,buf,cur,csel);
        }
        if (just&BIT(AKIRA_BTN_B)) {
            if (cur>0) { cur--; buf[cur]=0; draw_passentry(ssid,buf,cur,csel); }
            else return false;
        }
        if (just&BIT(AKIRA_BTN_X)) {
            memcpy(out,buf,cur); out[cur]=0; return true;
        }
    }
}

static void do_connect(const char *ssid, const char *pass) {
    LOG_INF("WiFi connect SSID:%s",ssid);
#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
    struct net_if *iface=net_if_get_default();
    struct wifi_connect_req_params p={
        .ssid=(const uint8_t*)ssid, .ssid_length=(uint8_t)strlen(ssid),
        .psk=(const uint8_t*)pass,  .psk_length=(uint8_t)strlen(pass),
        .security=strlen(pass)?WIFI_SECURITY_TYPE_PSK:WIFI_SECURITY_TYPE_NONE,
        .channel=WIFI_CHANNEL_ANY,
    };
    int r=net_mgmt(NET_REQUEST_WIFI_CONNECT,iface,&p,sizeof(p));
    if (r<0) LOG_ERR("connect:%d",r);
    else { akira_settings_set(AKIRA_SETTINGS_WIFI_SSID_KEY,ssid,0); akira_settings_set(AKIRA_SETTINGS_WIFI_PSK_KEY,pass,0); }
#endif
}

static void do_scan(void) {
    g_ssid_count=0;
#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
    if (!g_wifi_cb_reg) {
        k_sem_init(&g_scan_done,0,1);
        net_mgmt_init_event_callback(&g_wifi_cb,on_wifi_event,
            NET_EVENT_WIFI_SCAN_RESULT|NET_EVENT_WIFI_SCAN_DONE|NET_EVENT_WIFI_CONNECT_RESULT);
        net_mgmt_add_event_callback(&g_wifi_cb); g_wifi_cb_reg=true;
    }
    g_scan_count=0; k_sem_reset(&g_scan_done);
    struct net_if *iface=net_if_get_default();
    int r=net_mgmt(NET_REQUEST_WIFI_SCAN,iface,NULL,0);
    if (r<0) { LOG_ERR("scan:%d",r); return; }
    k_sem_take(&g_scan_done,K_SECONDS(10));
    for (int i=0;i<g_scan_count&&i<16;i++){
        int l=MIN((int)g_scan[i].ssid_length,(int)SSID_MAX-1);
        memcpy(g_ssid_list[g_ssid_count],g_scan[i].ssid,l);
        g_ssid_list[g_ssid_count][l]=0;
        g_ssid_count++;
    }
#else
    strncpy(g_ssid_list[0],"WiFi not available",SSID_MAX-1); g_ssid_count=1;
#endif
}

static void draw_list(const char *status) {
    akira_display_clear(C_BLACK); lc_header("WIFI");
    akira_display_text(ITEM_X+4,LIST_Y+4,status,C_GRAY);
    int vis=(FOOT_Y-LIST_Y-18)/ITEM_H;
    if (g_scroll>g_ssid_count-vis) g_scroll=g_ssid_count>vis?g_ssid_count-vis:0;
    if (g_scroll<0) g_scroll=0;
    for (int i=g_scroll;i<g_ssid_count&&i<g_scroll+vis;i++)
        lc_item(i-g_scroll,g_sel-g_scroll,g_ssid_list[i],"");
    lc_footer("A-Connect  |  B-Back");
    akira_display_flush();
}

void wifi_screen_load(void)
{
    extern void settings_screen_load(void);
    g_sel=0; g_scroll=0;
    draw_list("Scanning...");
    do_scan();
    draw_list(g_ssid_count?"Tap A to connect":"No networks found");
    uint32_t prev=0;
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns=akira_input_get_bitmask(), just=btns&~prev; prev=btns;
        if (!just) continue;
        int vis=(FOOT_Y-LIST_Y-18)/ITEM_H;
        if (just&BIT(AKIRA_BTN_UP)) { if(g_sel>0){g_sel--;if(g_sel<g_scroll)g_scroll--;draw_list("Select network");} }
        if (just&BIT(AKIRA_BTN_DOWN)) { if(g_sel<g_ssid_count-1){g_sel++;if(g_sel>=g_scroll+vis)g_scroll++;draw_list("Select network");} }
        if ((just&BIT(AKIRA_BTN_A)) && g_ssid_count>0) {
            char pass[PASS_MAX]={0};
            if (enter_password(g_ssid_list[g_sel],pass,PASS_MAX)) {
                do_connect(g_ssid_list[g_sel],pass);
                draw_list("Connecting...");
            } else {
                draw_list("Cancelled");
            }
        }
        if ((just&BIT(AKIRA_BTN_B))||(just&BIT(AKIRA_BTN_HOME))) { settings_screen_load(); return; }
    }
}
