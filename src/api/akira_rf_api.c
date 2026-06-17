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
            n = h->ops->recv(h, s_rf_poll_buf.data, RF_RX_MAX_PACKET, 250);
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

#endif /* CONFIG_WIFI && CONFIG_AKIRA_RF_FRAMEWORK */

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
