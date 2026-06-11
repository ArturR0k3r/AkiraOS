/**
 * @file akira_rf_api.c
 * @brief RF API implementation for WASM exports
 */

#include "akira_api.h"
#include "akira_rf_api.h"
#include <runtime/security.h>
#include <zephyr/logging/log.h>
#include "../drivers/rf/rf_framework.h"
#include <lib/mem_helper.h>
#include <string.h>
#include <zephyr/sys/util.h>

#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#endif

LOG_MODULE_REGISTER(akira_rf_api, CONFIG_AKIRA_LOG_LEVEL);

/* Protects g_active_chip, g_active_driver, and rf_framework_initialized.
 * RF ops may be called concurrently if two WASM apps both hold rf.transceive. */
static K_MUTEX_DEFINE(s_rf_lock);

static akira_rf_chip_t g_active_chip = AKIRA_RF_CHIP_NONE;
#ifdef CONFIG_AKIRA_RF_FRAMEWORK
static bool rf_framework_initialized = false;
/* Tracks which chips have been hardware-initialized, indexed by akira_rf_chip_t.
 * Lets akira_rf_select() init a chip once and then flip the active pointer on
 * later switches, preserving each chip's register config. */
static bool s_inited[AKIRA_RF_CHIP_LR2021 + 1];
#endif /* CONFIG_AKIRA_RF_FRAMEWORK */
static const struct akira_rf_driver *g_active_driver = NULL;

/* ===========================================================================
 * RX daemon + packet queue
 * ===========================================================================
 * One background thread polls the active driver and enqueues received packets.
 * Callers use akira_rf_recv_pop() instead of the blocking akira_rf_receive().
 */
#ifdef CONFIG_AKIRA_RF_RX_DAEMON


#define AKIRA_DAEMOM_SLEEP_MS 100
#define RF_RX_MAX_PACKET 255

struct rf_rx_packet {
    uint8_t  data[RF_RX_MAX_PACKET];
    uint16_t len;
};

K_MSGQ_DEFINE(s_rx_msgq, sizeof(struct rf_rx_packet),
              CONFIG_AKIRA_RF_RX_QUEUE_DEPTH, 4);

/* Static buffer — keeps 257 bytes off the daemon's stack. */
static struct rf_rx_packet s_rx_poll_buf;

static void rf_rx_daemon_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    while (1) {
        /* Acquire the RF lock non-blocking. If a TX or manual recv is in
         * progress, skip this cycle — don't fight over chip state. */
        if (k_mutex_lock(&s_rf_lock, K_NO_WAIT) != 0) {
            LOG_DBG("daemon: lock busy, skip [t=%lld ms]", k_uptime_get());
            k_msleep(AKIRA_DAEMOM_SLEEP_MS);
            continue;
        }

        const struct akira_rf_driver *drv = g_active_driver;
        if (!drv || !drv->rx) {
            LOG_DBG("daemon: no active driver [t=%lld ms]", k_uptime_get());
            k_mutex_unlock(&s_rf_lock);
            k_msleep(AKIRA_DAEMOM_SLEEP_MS);
            continue;
        }

        LOG_DBG("daemon: polling %s [t=%lld ms]", drv->name, k_uptime_get());

        /* Lock is held for the full poll so TX and manual recv cannot
         * interleave with the daemon's SetRx/IRQ-poll/ReadFifo sequence. */
        int n = drv->rx(s_rx_poll_buf.data, RF_RX_MAX_PACKET,
                        CONFIG_AKIRA_RF_POLL_INTERVAL_MS);
        k_mutex_unlock(&s_rf_lock);

        LOG_DBG("daemon: poll done n=%d [t=%lld ms]", n, k_uptime_get());

        /* Sleep between cycles whether or not a packet arrived. When a packet
         * was received the driver already consumed the POLL_INTERVAL window
         * internally, so no extra sleep needed — but an empty poll returns
         * immediately and must yield before hammering the bus again. */
        if (n <= 0) {
            k_msleep(AKIRA_DAEMOM_SLEEP_MS);
        }

        if (n > 0) {
            s_rx_poll_buf.len = (uint16_t)n;
            if (k_msgq_put(&s_rx_msgq, &s_rx_poll_buf, K_NO_WAIT) == -ENOMSG) {
                /* Queue full — evict oldest, then enqueue new packet */
                struct rf_rx_packet discard;
                k_msgq_get(&s_rx_msgq, &discard, K_NO_WAIT);
                k_msgq_put(&s_rx_msgq, &s_rx_poll_buf, K_NO_WAIT);
                LOG_DBG("RF RX queue full — dropped oldest packet");
            }
        }
    }
}

K_THREAD_DEFINE(rf_rx_daemon, CONFIG_AKIRA_RF_DAEMON_STACK_SIZE,
                rf_rx_daemon_fn, NULL, NULL, NULL,
                CONFIG_AKIRA_RF_DAEMON_PRIORITY, 0, 0);

#endif /* CONFIG_AKIRA_RF_RX_DAEMON */

/* Ensure RF framework is initialized */
#ifdef CONFIG_AKIRA_RF_FRAMEWORK
static int ensure_rf_framework(void)
{
    /* Called under s_rf_lock */
    if (rf_framework_initialized)
    {
        return 0;
    }

    int ret = rf_framework_init();
    if (ret < 0)
    {
        LOG_ERR("RF framework init failed: %d", ret);
        return ret;
    }

    rf_framework_initialized = true;
    return 0;
}

/* Map the public chip enum to the RF framework chip type. */
static int map_chip_type(akira_rf_chip_t chip, rf_chip_type_t *out)
{
    switch (chip)
    {
    case AKIRA_RF_CHIP_LR2021:
        *out = RF_CHIP_LR2021;
        return 0;
    case AKIRA_RF_CHIP_CC1121:
        *out = RF_CHIP_CC1121;
        return 0;
    case AKIRA_RF_CHIP_LR1121:
        *out = RF_CHIP_LR1121;
        return 0;
    case AKIRA_RF_CHIP_CC1101:
        *out = RF_CHIP_CC1101;
        return 0;
    case AKIRA_RF_CHIP_NRF24L01:
        *out = RF_CHIP_NRF24L01;
        return 0;
    default:
        return -EINVAL;
    }
}
#endif /* CONFIG_AKIRA_RF_FRAMEWORK */

/* Core RF API functions (no security checks) */

int akira_rf_init(akira_rf_chip_t chip)
{
    LOG_INF("RF init: chip=%d", chip);

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    int ret;

    if (k_mutex_lock(&s_rf_lock, K_MSEC(2000)) != 0)
    {
        LOG_WRN("rf_init: lock timed out");
        return -EBUSY;
    }

    /* Ensure framework is initialized (under lock) */
    ret = ensure_rf_framework();
    if (ret < 0)
    {
        k_mutex_unlock(&s_rf_lock);
        return ret;
    }

    /* Map chip enum to RF framework type */
    rf_chip_type_t rf_type;
    if (map_chip_type(chip, &rf_type) < 0)
    {
        LOG_ERR("Unsupported chip type: %d", chip);
        k_mutex_unlock(&s_rf_lock);
        return -EINVAL;
    }

    /* Get driver from framework */
    const struct akira_rf_driver *driver = rf_framework_get_driver(rf_type);
    if (!driver)
    {
        LOG_ERR("Driver not found for chip type %d", rf_type);
        k_mutex_unlock(&s_rf_lock);
        return -ENODEV;
    }

    /* Initialize the driver */
    ret = driver->init();
    if (ret < 0)
    {
        LOG_ERR("Driver init failed: %d", ret);
        k_mutex_unlock(&s_rf_lock);
        return ret;
    }

    /* Store active driver and chip */
    s_inited[chip] = true;
    g_active_driver = driver;
    g_active_chip = chip;
    rf_framework_set_active_driver(rf_type);

    k_mutex_unlock(&s_rf_lock);
    LOG_INF("RF driver '%s' initialized successfully", driver->name);
    return 0;
#else
    (void)chip;
    return -ENOSYS;
#endif
}

int akira_rf_deinit(void)
{
    LOG_INF("RF deinit");

    if (k_mutex_lock(&s_rf_lock, K_MSEC(2000)) != 0)
    {
        LOG_WRN("rf_deinit: lock timed out");
        return -EBUSY;
    }
#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    if (g_active_driver && g_active_driver->deinit)
    {
        g_active_driver->deinit();
    }
    g_active_driver = NULL;
    rf_framework_set_active_driver(RF_CHIP_NONE);
    if (g_active_chip != AKIRA_RF_CHIP_NONE)
    {
        s_inited[g_active_chip] = false;
    }
#endif
    g_active_chip = AKIRA_RF_CHIP_NONE;
    k_mutex_unlock(&s_rf_lock);
    return 0;
}

int akira_rf_recv_pop(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
#ifdef CONFIG_AKIRA_RF_RX_DAEMON
    if (!buf || max_len == 0) {
        return -EINVAL;
    }

    struct rf_rx_packet pkt;
    k_timeout_t t = (timeout_ms == 0) ? K_NO_WAIT : K_MSEC(timeout_ms);
    int ret = k_msgq_get(&s_rx_msgq, &pkt, t);
    if (ret < 0) {
        return ret;
    }

    size_t copy = MIN(pkt.len, max_len);
    memcpy(buf, pkt.data, copy);
    return (int)copy;
#else
    (void)buf; (void)max_len; (void)timeout_ms;
    return -ENOSYS;
#endif
}

int akira_rf_select(akira_rf_chip_t chip)
{
    LOG_INF("RF select: chip=%d", chip);

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    int ret;

    if (chip == AKIRA_RF_CHIP_NONE)
    {
        return -EINVAL;
    }

    if (k_mutex_lock(&s_rf_lock, K_MSEC(2000)) != 0)
    {
        LOG_WRN("rf_select: lock timed out");
        return -EBUSY;
    }

    if (chip == g_active_chip)
    {
        k_mutex_unlock(&s_rf_lock);
        return 0;
    }

    ret = ensure_rf_framework();
    if (ret < 0)
    {
        k_mutex_unlock(&s_rf_lock);
        return ret;
    }

    rf_chip_type_t rf_type;
    if (map_chip_type(chip, &rf_type) < 0)
    {
        LOG_ERR("Unsupported chip type: %d", chip);
        k_mutex_unlock(&s_rf_lock);
        return -EINVAL;
    }

    const struct akira_rf_driver *driver = rf_framework_get_driver(rf_type);
    if (!driver)
    {
        LOG_ERR("Driver not found for chip type %d", rf_type);
        k_mutex_unlock(&s_rf_lock);
        return -ENODEV;
    }

    /* Hardware-init the chip only on its first selection */
    if (!s_inited[chip])
    {
        ret = driver->init();
        if (ret < 0)
        {
            LOG_ERR("Driver init failed: %d", ret);
            k_mutex_unlock(&s_rf_lock);
            return ret;
        }
        s_inited[chip] = true;
    }

    g_active_driver = driver;
    g_active_chip = chip;
    rf_framework_set_active_driver(rf_type);

    k_mutex_unlock(&s_rf_lock);
    LOG_INF("RF active chip set to '%s'", driver->name);
    return 0;
#else
    (void)chip;
    return -ENOSYS;
#endif
}

int akira_rf_send(const uint8_t *data, size_t len)
{
    if (k_mutex_lock(&s_rf_lock, K_MSEC(2000)) != 0)
    {
        LOG_WRN("rf_send: lock timed out");
        return -EBUSY;
    }
    if (g_active_chip == AKIRA_RF_CHIP_NONE || !g_active_driver)
    {
        k_mutex_unlock(&s_rf_lock);
        LOG_ERR("RF not initialized");
        return -ENODEV;
    }

    if (!data || len == 0)
    {
        k_mutex_unlock(&s_rf_lock);
        return -EINVAL;
    }

    LOG_DBG("RF send: %zu bytes", len);

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    if (!g_active_driver->tx)
    {
        k_mutex_unlock(&s_rf_lock);
        return -ENOSYS;
    }
    int ret = g_active_driver->tx(data, len);
    k_mutex_unlock(&s_rf_lock);
    return ret;
#else
    k_mutex_unlock(&s_rf_lock);
    (void)data;
    (void)len;
    return -ENOSYS;
#endif
}

int akira_rf_receive(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    if (k_mutex_lock(&s_rf_lock, K_MSEC(2000)) != 0)
    {
        LOG_WRN("rf_receive: lock timed out");
        return -EBUSY;
    }
    if (g_active_chip == AKIRA_RF_CHIP_NONE || !g_active_driver)
    {
        k_mutex_unlock(&s_rf_lock);
        LOG_ERR("RF not initialized");
        return -ENODEV;
    }

    if (!buffer || max_len == 0)
    {
        k_mutex_unlock(&s_rf_lock);
        return -EINVAL;
    }

    LOG_DBG("RF receive: max=%zu, timeout=%u", max_len, timeout_ms);

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    if (!g_active_driver->rx)
    {
        k_mutex_unlock(&s_rf_lock);
        return -ENOSYS;
    }
    int ret = g_active_driver->rx(buffer, max_len, timeout_ms);
    k_mutex_unlock(&s_rf_lock);
    return ret;
#else
    k_mutex_unlock(&s_rf_lock);
    (void)buffer;
    (void)max_len;
    (void)timeout_ms;
    return -ENOSYS;
#endif
}

int akira_rf_set_frequency(uint32_t freq_hz)
{
    LOG_INF("RF set frequency: %u Hz", freq_hz);

    if (k_mutex_lock(&s_rf_lock, K_MSEC(2000)) != 0)
    {
        LOG_WRN("rf_set_frequency: lock timed out");
        return -EBUSY;
    }
#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    if (!g_active_driver || !g_active_driver->set_frequency)
    {
        k_mutex_unlock(&s_rf_lock);
        return -ENOSYS;
    }
    int ret = g_active_driver->set_frequency(freq_hz);
    k_mutex_unlock(&s_rf_lock);
    return ret;
#else
    (void)freq_hz;
    k_mutex_unlock(&s_rf_lock);
    return -ENOSYS;
#endif
}

int akira_rf_set_power(int8_t dbm)
{
    LOG_INF("RF set power: %d dBm", dbm);

    if (k_mutex_lock(&s_rf_lock, K_MSEC(2000)) != 0)
    {
        LOG_WRN("rf_set_power: lock timed out");
        return -EBUSY;
    }
#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    if (!g_active_driver || !g_active_driver->set_power)
    {
        k_mutex_unlock(&s_rf_lock);
        return -ENOSYS;
    }
    int ret = g_active_driver->set_power(dbm);
    k_mutex_unlock(&s_rf_lock);
    return ret;
#else
    (void)dbm;
    k_mutex_unlock(&s_rf_lock);
    return -ENOSYS;
#endif
}

int akira_rf_get_rssi(int16_t *rssi)
{
    if (!rssi)
    {
        return -EINVAL;
    }

    if (k_mutex_lock(&s_rf_lock, K_MSEC(2000)) != 0)
    {
        LOG_WRN("rf_get_rssi: lock timed out");
        *rssi = -100;
        return -EBUSY;
    }
#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    if (!g_active_driver || !g_active_driver->get_rssi)
    {
        *rssi = -100;
        k_mutex_unlock(&s_rf_lock);
        return -ENOSYS;
    }
    int ret = g_active_driver->get_rssi(rssi);
    k_mutex_unlock(&s_rf_lock);
    return ret;
#else
    *rssi = -100;
    k_mutex_unlock(&s_rf_lock);
    return -ENOSYS;
#endif
}

#ifdef CONFIG_AKIRA_WASM_RUNTIME

/* WASM Native export API */

int akira_native_rf_select(wasm_exec_env_t exec_env, int chip)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    return akira_rf_select((akira_rf_chip_t)chip);
#else
    (void)chip;
    return -ENOSYS;
#endif
}

int akira_native_rf_recv_pop(wasm_exec_env_t exec_env, uint32_t buf_ptr,
                             uint32_t max_len, uint32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;

    if (max_len == 0)
        return -1;

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

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    return akira_rf_send(ptr, len);
#else
    (void)ptr;
    (void)len;
    return -ENOSYS;
#endif
}

int akira_native_rf_receive(wasm_exec_env_t exec_env, uint32_t buffer_ptr, uint32_t max_len, uint32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;

    if (max_len == 0)
        return -1;

    uint8_t *ptr = (uint8_t *)wasm_runtime_addr_app_to_native(module_inst, buffer_ptr);
    if (!ptr)
        return -EFAULT;

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    return akira_rf_receive(ptr, max_len, timeout_ms);
#else
    (void)ptr;
    (void)max_len;
    (void)timeout_ms;
    return -ENOSYS;
#endif
}

int akira_native_rf_set_frequency(wasm_exec_env_t exec_env, uint32_t freq_hz)
{

    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    return akira_rf_set_frequency(freq_hz);
#else
    (void)freq_hz;
    return -ENOSYS;
#endif
}

int akira_native_rf_get_rssi(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    int16_t rssi = 0;
    int ret = akira_rf_get_rssi(&rssi);
    if (ret < 0) {
        return ret;
    }
    return (int32_t)rssi;
#else
    return -ENOSYS;
#endif
}

int akira_native_rf_set_power(wasm_exec_env_t exec_env, int8_t dbm)
{

    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    return akira_rf_set_power(dbm);
#else
    (void)dbm;
    return -ENOSYS;
#endif
}

#ifdef CONFIG_WIFI

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
            /* Keep the strongest RSSI per channel */
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

    /* Reset scan context */
    (void)memset(g_wifi_scan_ctx.rssi, -100, sizeof(g_wifi_scan_ctx.rssi));
    (void)memset(g_wifi_scan_ctx.seen, 0, sizeof(g_wifi_scan_ctx.seen));
    k_sem_init(&g_wifi_scan_ctx.done, 0, 1);

    /* Register net_mgmt callback if not already registered */
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

    /* Wait for scan to complete */
    ret = k_sem_take(&g_wifi_scan_ctx.done, K_MSEC(WIFI_SCAN_TIMEOUT_MS));
    if (ret) {
        LOG_WRN("WiFi scan timed out, returning partial results");
    }

    /* Copy results to WASM buffer */
    for (int i = 0; i < WIFI_SCAN_MAX_CHANNELS; i++) {
        dst[i] = g_wifi_scan_ctx.rssi[i];
    }

    return WIFI_SCAN_MAX_CHANNELS;
}

#endif /* CONFIG_WIFI */

#endif /* CONFIG_AKIRA_WASM_RUNTIME */