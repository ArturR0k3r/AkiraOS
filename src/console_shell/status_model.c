/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_status_model
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_status_model, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file status_model.c
 * @brief Status-bar model (see status_model.h).  Moved out of home_screen.c
 *        so every screen's status bar shows the same live state.
 */

#include "status_model.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <stdio.h>
#include <string.h>
#include <lib/akira_time.h>

#if defined(CONFIG_SNTP)
#include <zephyr/net/sntp.h>
#endif

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#endif

#ifdef CONFIG_BT
#include <connectivity/bluetooth/bt_manager.h>
#endif

static shell_status_t g_status;

/* ---- SNTP: one-shot sync the first time Wi-Fi comes up ------------- */

#if defined(CONFIG_SNTP)
/* Wait 8 s after WiFi connect — gives DHCP + DNS time to settle fully */
#define SNTP_INITIAL_DELAY_S 8
#define SNTP_MAX_RETRIES 5

/* Fallback NTP server IP (Google) used when DNS resolution of the
 * hostname fails (-ENOENT from sntp_simple).  No DNS needed for this. */
#define SNTP_FALLBACK_IP "216.239.35.0"

static void sntp_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_sntp_work, sntp_work_handler);
static uint8_t g_sntp_retries;
static bool g_sntp_done; /* true once SNTP sync succeeded or gave up */

/* The handler runs on the system workqueue, and akira_time_set_epoch()
 * persists through akira_settings_set(), which queues onto that same workqueue
 * and waits — calling it from the handler would deadlock the queue.  So the
 * handler only records the result; status_model_update() applies it on the
 * shell thread. */
static int64_t g_sntp_epoch;
static atomic_t g_sntp_ready;

static void sntp_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (g_sntp_done) {
        return;
    }
    struct sntp_time st;

    /* First try the hostname; if DNS fails fall back to IP immediately */
    int r = sntp_simple("pool.ntp.org", 4000, &st);
    if (r == -ENOENT) {
        LOG_WRN("SNTP DNS failed, trying fallback IP " SNTP_FALLBACK_IP);
        r = sntp_simple(SNTP_FALLBACK_IP, 4000, &st);
    }

    if (r == 0) {
        g_sntp_epoch = (int64_t)st.seconds;
        atomic_set(&g_sntp_ready, 1);
        LOG_INF("SNTP sync OK: epoch %lld", (long long)st.seconds);
        g_sntp_done = true;
        return;
    }

    g_sntp_retries++;
    LOG_WRN("SNTP sync failed (%u/%u): %d", g_sntp_retries, SNTP_MAX_RETRIES, r);
    if (g_sntp_retries < SNTP_MAX_RETRIES) {
        /* Exponential backoff: 10 s, 20 s, 40 s, 60 s */
        static const uint8_t delays_s[] = {10, 20, 40, 60};
        uint8_t idx = MIN(g_sntp_retries - 1, ARRAY_SIZE(delays_s) - 1);

        k_work_reschedule(&g_sntp_work, K_SECONDS(delays_s[idx]));
    } else {
        g_sntp_done = true; /* give up after max retries */
    }
}
#endif /* CONFIG_SNTP */

/* ---- model ---------------------------------------------------------- */

static void format_clock(char out[6], bool *time_set)
{
    int64_t epoch = akira_time_get_epoch();

    *time_set = akira_time_is_set();
    if (*time_set) {
        /* Apply stored UTC offset so the bar shows local time */
        epoch += (int64_t)akira_time_get_tz_offset_s();
    }
    int64_t day_sec = epoch % 86400;

    if (day_sec < 0) {
        day_sec += 86400;
    }
    snprintf(out, 6, "%02u:%02u", (unsigned)(day_sec / 3600),
             (unsigned)((day_sec % 3600) / 60));
}

bool status_model_update(void)
{
    shell_status_t prev = g_status;

#if defined(CONFIG_SNTP)
    if (atomic_cas(&g_sntp_ready, 1, 0)) {
        akira_time_set_epoch(g_sntp_epoch);
    }
#endif

    format_clock(g_status.clock, &g_status.time_set);

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
    {
        struct net_if *iface = net_if_get_default();
        struct wifi_iface_status wst = {0};

        g_status.wifi_on = iface &&
                           net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface,
                                    &wst, sizeof(wst)) == 0 &&
                           wst.state >= WIFI_STATE_ASSOCIATED;
    }
#if defined(CONFIG_SNTP)
    /* Auto SNTP: schedule async when WiFi first connects and time not set */
    if (g_status.wifi_on && !prev.wifi_on && !g_sntp_done && !akira_time_is_set()) {
        g_sntp_retries = 0;
        k_work_reschedule(&g_sntp_work, K_SECONDS(SNTP_INITIAL_DELAY_S));
    }
    /* Reset on disconnect so we retry on the next connection */
    if (!g_status.wifi_on && prev.wifi_on) {
        k_work_cancel_delayable(&g_sntp_work);
        if (!g_sntp_done) {
            g_sntp_retries = 0;
        }
    }
#endif
#endif

#ifdef CONFIG_BT
    g_status.bt_on = (bt_manager_get_state() == BT_STATE_CONNECTED);
#endif

    status_model_update_battery();

    return strcmp(prev.clock, g_status.clock) != 0 ||
           prev.wifi_on != g_status.wifi_on || prev.bt_on != g_status.bt_on ||
           prev.batt_seq != g_status.batt_seq;
}

bool status_model_update_battery(void)
{
#ifdef CONFIG_AKIRA_POWER_MANAGER
    akira_battery_snapshot_t b;

    akira_pm_battery_snapshot(&b);
    if (b.seq == g_status.batt_seq && b.valid == g_status.batt_valid) {
        return false;
    }
    g_status.batt_valid = b.valid;
    g_status.batt_pct = b.pct;
    g_status.batt_state = (uint8_t)b.state;
    g_status.batt_online = b.online;
    g_status.batt_ma = b.current_ma;
    g_status.batt_mv = b.voltage_mv;
    g_status.batt_seq = b.seq;
    return true;
#else
    return false;
#endif
}

const shell_status_t *status_model_get(void)
{
    return &g_status;
}
