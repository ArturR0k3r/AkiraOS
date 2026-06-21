/**
 * @file akira_rf_api.c
 * @brief RF API — self-contained raw-RF stack for WASM/shell access
 */

#include "akira_api.h"
#include "akira_rf_api.h"
#include <runtime/security.h>
#include <zephyr/logging/log.h>
#include "connectivity/radio_interface.h"
#if defined(CONFIG_AKIRA_LR2021)
#include "../drivers/rf/lr2021.h"
#endif
#if defined(CONFIG_AKIRA_CC1121)
#include "../drivers/rf/cc1121.h"
#endif
#if defined(CONFIG_AKIRA_LR1121)
#include "../drivers/rf/lr1121.h"
#endif
#if defined(CONFIG_AKIRA_CC1101)
#include "../drivers/rf/cc1101.h"
#endif
#if defined(CONFIG_AKIRA_NRF24L01)
#include "../drivers/rf/nrf24l01.h"
#endif
#include <lib/mem_helper.h>
#include <string.h>
#include <zephyr/sys/util.h>

#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
#include <esp_wifi.h>
#endif
#endif

LOG_MODULE_REGISTER(akira_rf_api, CONFIG_AKIRA_LOG_LEVEL);

/* Serializes init/select/deinit AND data-path bus ops (single operator). */
static K_MUTEX_DEFINE(s_chip_lock);

#define CHIP_LOCK_TIMEOUT_MS 2000

/* RX queue: the daemon fills it; akira_rf_receive() drains it (decoupled from
 * the chip). Shared, so defined here ahead of both users. */
#define RF_RX_MAX_PACKET    255
struct rf_rx_packet { uint8_t data[RF_RX_MAX_PACKET]; uint16_t len; };
K_MSGQ_DEFINE(s_rf_rx_msgq, sizeof(struct rf_rx_packet),
              CONFIG_AKIRA_RF_RX_QUEUE_DEPTH, 4);

static akira_rf_chip_t g_active_chip = AKIRA_RF_CHIP_NONE;
static radio_handle_t *g_active_handle = NULL;
/* Per-chip init tracking: init hardware once, then just swap active handle. */
static bool s_inited[AKIRA_RF_CHIP_MAX];

static radio_handle_t *map_chip_to_handle(akira_rf_chip_t chip)
{
    switch (chip) {
#if defined(CONFIG_AKIRA_LR2021)
    case AKIRA_RF_CHIP_LR2021:   return lr2021_get_handle();
#endif
#if defined(CONFIG_AKIRA_CC1121)
    case AKIRA_RF_CHIP_CC1121:   return cc1121_get_handle();
#endif
#if defined(CONFIG_AKIRA_LR1121)
    case AKIRA_RF_CHIP_LR1121:   return lr1121_get_handle();
#endif
#if defined(CONFIG_AKIRA_CC1101)
    case AKIRA_RF_CHIP_CC1101:   return cc1101_get_handle();
#endif
#if defined(CONFIG_AKIRA_NRF24L01)
    case AKIRA_RF_CHIP_NRF24L01: return nrf24l01_get_handle();
#endif
    default:                      return NULL;
    }
}

int akira_rf_init(akira_rf_chip_t chip)
{
    LOG_INF("RF init: chip=%d", chip);

    radio_handle_t *handle = map_chip_to_handle(chip);
    if (!handle) {
        LOG_ERR("Unsupported chip type: %d", chip);
        return -EINVAL;
    }

    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        return -EBUSY;
    }

    if (!handle->ops) {
        LOG_ERR("Radio %d has no ops (stub)", chip);
        k_mutex_unlock(&s_chip_lock);
        return -ENODEV;
    }

    int ret = 0;
    if (handle->ops->init) {
        ret = handle->ops->init(handle);
    }
    if (ret < 0) {
        LOG_ERR("Radio '%s' init failed: %d", handle->name, ret);
        k_mutex_unlock(&s_chip_lock);
        return ret;
    }

    int aret = radio_manager_acquire(handle, "rf");
    if (aret < 0) {
        LOG_ERR("Radio '%s' busy (owned elsewhere): %d", handle->name, aret);
        k_mutex_unlock(&s_chip_lock);
        return aret;
    }
    s_inited[chip] = true;
    g_active_chip = chip;
    g_active_handle = handle;

    k_mutex_unlock(&s_chip_lock);
    LOG_INF("RF radio '%s' initialized", handle->name);
    return 0;
}

int akira_rf_deinit(void)
{
    LOG_INF("RF deinit");

    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        return -EBUSY;
    }

    radio_handle_t *h = g_active_handle;
    if (h && h->ops && h->ops->deinit) {
        h->ops->deinit(h);
    }
    if (g_active_chip != AKIRA_RF_CHIP_NONE) {
        s_inited[g_active_chip] = false;
    }
    if (g_active_handle) {
        radio_manager_release(g_active_handle, "rf");
        g_active_handle = NULL;
    }
    g_active_chip = AKIRA_RF_CHIP_NONE;

    k_mutex_unlock(&s_chip_lock);
    return 0;
}

int akira_rf_select(akira_rf_chip_t chip)
{
    LOG_INF("RF select: chip=%d", chip);

    if (chip == AKIRA_RF_CHIP_NONE) {
        return -EINVAL;
    }

    radio_handle_t *handle = map_chip_to_handle(chip);
    if (!handle) {
        LOG_ERR("Unsupported chip type: %d", chip);
        return -EINVAL;
    }

    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        return -EBUSY;
    }

    if (chip == g_active_chip) {
        k_mutex_unlock(&s_chip_lock);
        return 0;
    }

    /* Hardware-init the chip only on its first selection */
    if (!s_inited[chip]) {
        int ret = 0;
        if (handle->ops && handle->ops->init) {
            ret = handle->ops->init(handle);
        }
        if (ret < 0) {
            LOG_ERR("Radio %d init failed: %d", chip, ret);
            k_mutex_unlock(&s_chip_lock);
            return ret;
        }
        s_inited[chip] = true;
    }

    if (g_active_handle && g_active_handle != handle) {
        radio_manager_release(g_active_handle, "rf");
        g_active_handle = NULL;
        g_active_chip = AKIRA_RF_CHIP_NONE;
    }
    int aret = radio_manager_acquire(handle, "rf");
    if (aret < 0) {
        k_mutex_unlock(&s_chip_lock);
        return aret;
    }
    g_active_handle = handle;
    g_active_chip = chip;

    k_mutex_unlock(&s_chip_lock);
    LOG_INF("RF active radio set to chip=%d (%s)", chip, handle->name);
    return 0;
}

int akira_rf_send(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return -EINVAL;
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->send) ? h->ops->send(h, data, len) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_receive(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    if (!buf || max_len == 0) return -EINVAL;
    /* Pull from the daemon's RX queue rather than touching the chip directly —
     * the daemon owns the (continuous) RX path. Avoids racing the daemon for
     * the chip and works for both interrupt-driven and polling radios. */
    struct rf_rx_packet pkt;
    if (k_msgq_get(&s_rf_rx_msgq, &pkt, K_MSEC(timeout_ms)) != 0) {
        return 0;  /* timeout, no packet */
    }
    size_t n = (pkt.len < max_len) ? pkt.len : max_len;
    memcpy(buf, pkt.data, n);
    return (int)n;
}

int akira_rf_set_frequency(uint32_t freq_hz)
{
    LOG_INF("RF set frequency: %u Hz", freq_hz);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_frequency) ? h->ops->set_frequency(h, freq_hz) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_set_power(int8_t dbm)
{
    LOG_INF("RF set power: %d dBm", dbm);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_power) ? h->ops->set_power(h, dbm) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_set_modulation(radio_modulation_t mod)
{
    LOG_INF("RF set modulation: %d", mod);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_modulation) ? h->ops->set_modulation(h, mod) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_set_spreading_factor(uint8_t sf)
{
    LOG_INF("RF set LoRa SF: %u", sf);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_spreading_factor) ? h->ops->set_spreading_factor(h, sf) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_set_bandwidth(uint32_t bw_hz)
{
    LOG_INF("RF set BW: %u Hz", bw_hz);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_bandwidth) ? h->ops->set_bandwidth(h, bw_hz) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_set_coding_rate(uint8_t cr)
{
    LOG_INF("RF set LoRa CR: 4/%u", cr);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_coding_rate) ? h->ops->set_coding_rate(h, cr) : -ENODEV;
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_get_rssi(int16_t *rssi)
{
    if (!rssi) return -EINVAL;
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        *rssi = RADIO_RSSI_UNAVAILABLE; return -EBUSY;
    }
    radio_handle_t *h = g_active_handle;
    int ret;
    if (h && h->ops && h->ops->get_rssi) {
        ret = h->ops->get_rssi(h, rssi);
        if (ret == -ENOSYS) *rssi = RADIO_RSSI_UNAVAILABLE;
    } else {
        *rssi = RADIO_RSSI_UNAVAILABLE; ret = -ENODEV;
    }
    k_mutex_unlock(&s_chip_lock);
    return ret;
}

radio_handle_t *akira_rf_get_active_handle(void)
{
    return g_active_handle;
}

#ifdef CONFIG_AKIRA_RF_RX_DAEMON

#define RF_DAEMON_SLEEP_MS  100

static struct rf_rx_packet s_rf_poll_buf;

static void rf_rx_daemon_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    while (1) {
        if (!g_active_handle) { k_msleep(RF_DAEMON_SLEEP_MS); continue; }
        radio_handle_t *h = g_active_handle;
        if (!h || !h->ops || !h->ops->recv) { k_msleep(RF_DAEMON_SLEEP_MS); continue; }

        int n;
        if (h->ops->rx_wait) {
            /* Interrupt-driven continuous RX. recv() (under the lock, briefly)
             * arms continuous RX on first call and reads any pending packet
             * non-blocking. rx_wait() then blocks LOCK-FREE on the IRQ until the
             * next packet — the chip never leaves RX (no gap) and the lock is
             * free while idle (no starvation). */
            if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
                k_msleep(RF_DAEMON_SLEEP_MS); continue;
            }
            n = h->ops->recv(h, s_rf_poll_buf.data, RF_RX_MAX_PACKET, 0);
            k_mutex_unlock(&s_chip_lock);
            if (n <= 0) { h->ops->rx_wait(h, 1000); continue; }  /* wait next IRQ */
        } else {
            /* Polling radios: short window so the lock is released frequently. */
            if (k_mutex_lock(&s_chip_lock, K_NO_WAIT) != 0) {
                k_msleep(RF_DAEMON_SLEEP_MS); continue;
            }
            n = h->ops->recv(h, s_rf_poll_buf.data, RF_RX_MAX_PACKET, 2000);
            k_mutex_unlock(&s_chip_lock);
            if (n <= 0) { k_msleep(RF_DAEMON_SLEEP_MS); continue; }
        }
        LOG_INF("RF RX %d bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                n,
                n > 0 ? s_rf_poll_buf.data[0] : 0, n > 1 ? s_rf_poll_buf.data[1] : 0,
                n > 2 ? s_rf_poll_buf.data[2] : 0, n > 3 ? s_rf_poll_buf.data[3] : 0,
                n > 4 ? s_rf_poll_buf.data[4] : 0, n > 5 ? s_rf_poll_buf.data[5] : 0,
                n > 6 ? s_rf_poll_buf.data[6] : 0, n > 7 ? s_rf_poll_buf.data[7] : 0);
        s_rf_poll_buf.len = (uint16_t)n;
        if (k_msgq_put(&s_rf_rx_msgq, &s_rf_poll_buf, K_NO_WAIT) == -ENOMSG) {
            struct rf_rx_packet discard;
            k_msgq_get(&s_rf_rx_msgq, &discard, K_NO_WAIT);
            k_msgq_put(&s_rf_rx_msgq, &s_rf_poll_buf, K_NO_WAIT);
        }
    }
}

K_THREAD_DEFINE(rf_rx_daemon, CONFIG_AKIRA_RF_DAEMON_STACK_SIZE,
                rf_rx_daemon_fn, NULL, NULL, NULL,
                CONFIG_AKIRA_RF_DAEMON_PRIORITY, 0, 0);

int akira_rf_recv_pop(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    if (!buf || max_len == 0) return -EINVAL;
    struct rf_rx_packet pkt;
    k_timeout_t t = (timeout_ms == 0) ? K_NO_WAIT : K_MSEC(timeout_ms);
    int ret = k_msgq_get(&s_rf_rx_msgq, &pkt, t);
    if (ret < 0) return ret;
    size_t copy = MIN(pkt.len, max_len);
    memcpy(buf, pkt.data, copy);
    return (int)copy;
}

#else
int akira_rf_recv_pop(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    ARG_UNUSED(buf); ARG_UNUSED(max_len); ARG_UNUSED(timeout_ms);
    return -ENOSYS;
}
#endif /* CONFIG_AKIRA_RF_RX_DAEMON */

#ifdef CONFIG_AKIRA_WASM_RUNTIME

/* WASM Native export API */

int akira_native_rf_select(wasm_exec_env_t exec_env, int chip)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    return akira_rf_select((akira_rf_chip_t)chip);
}

int akira_native_rf_recv_pop(wasm_exec_env_t exec_env, uint32_t buf_ptr,
                             uint32_t max_len, uint32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;

    if (max_len == 0)
        return -EINVAL;

    uint8_t *buf = (uint8_t *)wasm_runtime_addr_app_to_native(module_inst, buf_ptr);
    if (!buf)
        return -EFAULT;

    return akira_rf_recv_pop(buf, (size_t)max_len, timeout_ms);
}

int akira_native_rf_send(wasm_exec_env_t exec_env, uint32_t payload_ptr, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;

    uint8_t *ptr = (uint8_t *)wasm_runtime_addr_app_to_native(module_inst, payload_ptr);
    if (!ptr)
        return -EFAULT;

    return akira_rf_send(ptr, len);
}

int akira_native_rf_receive(wasm_exec_env_t exec_env, uint32_t buffer_ptr,
                             uint32_t max_len, uint32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;

    if (max_len == 0)
        return -EINVAL;

    uint8_t *ptr = (uint8_t *)wasm_runtime_addr_app_to_native(module_inst, buffer_ptr);
    if (!ptr)
        return -EFAULT;

    return akira_rf_receive(ptr, max_len, timeout_ms);
}

int akira_native_rf_set_frequency(wasm_exec_env_t exec_env, uint32_t freq_hz)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    return akira_rf_set_frequency(freq_hz);
}

int akira_native_rf_set_modulation(wasm_exec_env_t exec_env, int mod)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    return akira_rf_set_modulation((radio_modulation_t)mod);
}

int akira_native_rf_set_spreading_factor(wasm_exec_env_t exec_env, int sf)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    if (sf < 6 || sf > 12) return -EINVAL;
    return akira_rf_set_spreading_factor((uint8_t)sf);
}

int akira_native_rf_set_bandwidth(wasm_exec_env_t exec_env, uint32_t bw_hz)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    return akira_rf_set_bandwidth(bw_hz);
}

int akira_native_rf_set_coding_rate(wasm_exec_env_t exec_env, int cr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    if (cr < 5 || cr > 8) return -EINVAL;
    return akira_rf_set_coding_rate((uint8_t)cr);
}

int akira_native_rf_get_rssi(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    int16_t rssi = 0;
    int ret = akira_rf_get_rssi(&rssi);
    if (ret < 0) {
        return ret;
    }
    return (int32_t)rssi;
}

int akira_native_rf_set_power(wasm_exec_env_t exec_env, int8_t dbm)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    return akira_rf_set_power(dbm);
}

#if defined(CONFIG_WIFI) && defined(CONFIG_AKIRA_RF_FRAMEWORK)

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

/*
 * Wire struct — layout MUST stay in sync with akira_wifi_ap_t in akira_api.h.
 * Natural alignment gives 48 bytes; verified with BUILD_ASSERT below.
 */
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

static struct wifi_ap_wire              g_aps_buf[WIFI_APS_MAX];
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

        /* Deduplicate by BSSID; update RSSI if this reading is stronger */
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

/*
 * wifi_scan_aps(buf, buf_len) → int
 *
 * Runs a passive 802.11 scan and fills buf with akira_wifi_ap_t records.
 * Blocks up to WIFI_APS_TIMEOUT_MS ms. Returns the number of APs found,
 * or a negative errno on failure.
 *
 * Type string: "(*~)i" — WAMR validates ptr+len against WASM linear memory
 * and converts the WASM offset to a native pointer before the call.
 */
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

/* ── 802.11 deauthentication frame injector ─────────────────────────────── */

/*
 * 802.11 Management frame: Deauthentication (subtype 0xC).
 * 24-byte fixed layout per IEEE 802.11-2020 §9.3.3.1.
 */
struct __attribute__((packed)) deauth_frame {
    uint8_t  fc[2];      /* 0xC0 0x00 — Management, Deauthentication            */
    uint8_t  dur[2];     /* 0x3A 0x01 — NAV duration (~314 µs)                  */
    uint8_t  da[6];      /* Destination: target client or FF:FF:FF:FF:FF:FF      */
    uint8_t  sa[6];      /* Source: spoofed as AP BSSID                          */
    uint8_t  bssid[6];   /* BSS ID: AP BSSID                                     */
    uint8_t  seq[2];     /* Sequence control (incremented per frame)             */
    uint8_t  reason[2];  /* Reason code LE: 7 = Class-3 frame from non-assoc STA */
};

BUILD_ASSERT(sizeof(struct deauth_frame) == 24, "deauth_frame must be 24 bytes");

#define DEAUTH_MAX_COUNT      9999
#define DEAUTH_MIN_INTERVAL_MS  10

/*
 * wifi_deauth(bssid, client_mac, channel, count, interval_ms) → int
 *
 * Injects `count` 802.11 deauthentication frames directed at `client_mac`
 * (or broadcast FF:FF:FF:FF:FF:FF) from `bssid` on `channel`.
 * `interval_ms` is the inter-frame gap (min 10 ms).
 *
 * Returns the number of frames sent, or negative errno on error.
 * Requires AKIRA_CAP_WIFI_INJECT.
 *
 * Type string: "(**iii)i"
 */
int akira_native_wifi_deauth(wasm_exec_env_t exec_env,
                              void *bssid_ptr, void *client_ptr,
                              int32_t channel, int32_t count, int32_t interval_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_WIFI_INJECT, -EPERM);

    if (!bssid_ptr || !client_ptr)          return -EINVAL;
    if (channel < 1 || channel > 14)        return -EINVAL;
    if (count   < 1 || count > DEAUTH_MAX_COUNT) return -EINVAL;
    if (interval_ms < DEAUTH_MIN_INTERVAL_MS) interval_ms = DEAUTH_MIN_INTERVAL_MS;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, bssid_ptr,  6);
    WASM_ADDR_CHECK(inst, client_ptr, 6);

    const uint8_t *bssid  = (const uint8_t *)bssid_ptr;
    const uint8_t *client = (const uint8_t *)client_ptr;

    /* Set the radio to the target channel before injection */
    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);

    struct deauth_frame frame;
    frame.fc[0]     = 0xC0; frame.fc[1]     = 0x00;
    frame.dur[0]    = 0x3A; frame.dur[1]    = 0x01;
    frame.reason[0] = 0x07; frame.reason[1] = 0x00; /* Reason 7 */
    memcpy(frame.da,    client, 6);
    memcpy(frame.sa,    bssid,  6);
    memcpy(frame.bssid, bssid,  6);

    int sent = 0;
    for (int32_t i = 0; i < count; i++) {
        /* Increment sequence number — low 12 bits of seq[0:1], LE */
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

#endif /* CONFIG_WIFI && CONFIG_AKIRA_RF_FRAMEWORK */

/* ── Raw Sub-GHz OOK capture / replay ──────────────────────────────────── */

/*
 * These functions use the CC1101 (or CC1121) in "asynchronous serial mode":
 *
 *   IOCFG0 ← 0x0C  →  GDO0 outputs demodulated serial data (raw OOK)
 *   PKTCTRL0 ← 0x32 →  infinite packet length, no CRC, async serial mode
 *
 * Capture measures GPIO edge timings with a k_cycle_get_32() based loop.
 * Each uint16_t sample is a pulse duration in microseconds (mark or space),
 * alternating: samples[0] = first mark, samples[1] = first space, ...
 * A silence > RF_RAW_GAP_US signals end-of-burst.
 *
 * Replay drives the TX pin (GDO0 in TX mode) via GPIO bit-banging timed by
 * the same cycle counter.
 *
 * On unsupported chips (LoRa-only, WiFi) both functions return -ENOTSUP.
 */

#define RF_RAW_GAP_US       20000u  /* silence longer than this = end of burst */
#define RF_RAW_MAX_SAMPLES  4096u   /* hard cap regardless of WASM request     */
#define RF_RAW_MIN_PULSE_US    50u  /* shorter pulses rejected as noise        */
#define RF_RAW_MAX_REPEAT       9

int akira_native_rf_raw_capture(wasm_exec_env_t exec_env,
                                 uint32_t buf_wasm, uint32_t max_samples,
                                 int32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (max_samples == 0) return -EINVAL;
    if (timeout_ms  <= 0) timeout_ms = 5000;
    if (max_samples  > RF_RAW_MAX_SAMPLES) max_samples = RF_RAW_MAX_SAMPLES;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) return -EFAULT;

    /* Validate that the WASM buffer can hold max_samples uint16_t values */
    uint16_t *buf = (uint16_t *)wasm_runtime_addr_app_to_native(inst, buf_wasm);
    if (!buf) return -EFAULT;
    if (!wasm_runtime_validate_native_addr(inst, buf,
                                           max_samples * sizeof(uint16_t)))
        return -EFAULT;

#if defined(CONFIG_AKIRA_CC1101) || defined(CONFIG_AKIRA_CC1121)
    /*
     * TODO(hw): Configure CC1101/CC1121 for asynchronous raw OOK RX:
     *   cc1101_write_reg(CC1101_REG_IOCFG0,   0x0C); // GDO0 = serial data
     *   cc1101_write_reg(CC1101_REG_PKTCTRL0, 0x32); // infinite/async serial
     *   cc1101_write_reg(CC1101_REG_MDMCFG2,  0x30); // OOK, no sync/preamble
     *   cc1101_strobe(CC1101_CMD_SRX);
     *
     * Then sample GDO0 GPIO transitions with k_cycle_get_32() timing.
     * The loop below is the architectural skeleton — GPIO read is the stub.
     */

    uint32_t deadline = (uint32_t)k_uptime_get() + (uint32_t)timeout_ms;
    uint32_t n        = 0;
    int      last_lvl = -1;
    uint32_t t_start  = 0;

    while ((uint32_t)k_uptime_get() < deadline && n < max_samples) {
        /* TODO(hw): int lvl = gpio_pin_get(gdo0_dev, gdo0_pin); */
        int lvl = 0; /* stub: always low — replace with real GPIO read */

        if (last_lvl < 0) {
            /* Waiting for first edge */
            if (lvl) { last_lvl = lvl; t_start = k_cycle_get_32(); }
            continue;
        }

        if (lvl != last_lvl) {
            uint32_t now  = k_cycle_get_32();
            uint32_t diff_us = (uint32_t)k_cyc_to_us_floor32(now - t_start);

            if (diff_us > RF_RAW_GAP_US) break; /* end of burst */

            if (diff_us >= RF_RAW_MIN_PULSE_US && n < max_samples) {
                buf[n++] = (uint16_t)(diff_us > 65535u ? 65535u : diff_us);
            }
            last_lvl = lvl;
            t_start  = now;
        }
        /* Tight-loop on a realtime priority thread; yield every 100 µs */
        k_busy_wait(100);
    }

    /* TODO(hw): Return CC1101 to idle: cc1101_strobe(CC1101_CMD_SIDLE) */

    if (n == 0) return -ETIMEDOUT;
    LOG_INF("rf_raw_capture: %u samples", n);
    return (int)n;

#else
    ARG_UNUSED(buf);
    ARG_UNUSED(max_samples);
    ARG_UNUSED(timeout_ms);
    return -ENOTSUP;
#endif
}

int akira_native_rf_raw_replay(wasm_exec_env_t exec_env,
                                uint32_t buf_wasm, uint32_t sample_count,
                                int32_t repeat)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (sample_count == 0 || sample_count > RF_RAW_MAX_SAMPLES) return -EINVAL;
    if (repeat < 1) repeat = 1;
    if (repeat > RF_RAW_MAX_REPEAT) repeat = RF_RAW_MAX_REPEAT;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) return -EFAULT;

    const uint16_t *buf = (const uint16_t *)wasm_runtime_addr_app_to_native(
                                                inst, buf_wasm);
    if (!buf) return -EFAULT;
    if (!wasm_runtime_validate_native_addr(inst, (void *)buf,
                                           sample_count * sizeof(uint16_t)))
        return -EFAULT;

#if defined(CONFIG_AKIRA_CC1101) || defined(CONFIG_AKIRA_CC1121)
    /*
     * TODO(hw): Configure CC1101/CC1121 for async OOK TX:
     *   cc1101_write_reg(CC1101_REG_IOCFG0,   0x2D); // GDO0 = DCLK (sync serial TX)
     *   cc1101_write_reg(CC1101_REG_PKTCTRL0, 0x32); // async serial TX mode
     *   cc1101_strobe(CC1101_CMD_STX);
     *
     * Then GPIO bit-bang the TX data pin with the stored pulse timings.
     * Timing is driven by k_busy_wait(duration_us).
     */

    for (int r = 0; r < repeat; r++) {
        for (uint32_t i = 0; i < sample_count; i++) {
            uint32_t dur_us = buf[i];
            if (dur_us == 0) continue;
            int lvl = (i & 1) ? 0 : 1; /* even = mark, odd = space */

            /* TODO(hw): gpio_pin_set(tx_pin_dev, tx_pin, lvl); */
            ARG_UNUSED(lvl);
            k_busy_wait(dur_us);
        }
        /* TODO(hw): gpio_pin_set(tx_pin_dev, tx_pin, 0); — TX off between repeats */
        k_msleep(10); /* 10 ms gap between repetitions */
    }

    /* TODO(hw): cc1101_strobe(CC1101_CMD_SIDLE) */
    LOG_INF("rf_raw_replay: %u samples x%d", sample_count, repeat);
    return 0;

#else
    ARG_UNUSED(buf);
    ARG_UNUSED(sample_count);
    ARG_UNUSED(repeat);
    return -ENOTSUP;
#endif
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
