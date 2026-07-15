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
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(akira_rf_api, CONFIG_AKIRA_LOG_LEVEL);

/* Serializes init/select/deinit AND data-path bus ops (single operator). */
static K_MUTEX_DEFINE(s_chip_lock);

/* Cooperative daemon pause: while set, the RX daemon parks at the top of its
 * loop and never grabs the chip lock, so a raw capture/replay can own the chip
 * without racing it. This is a SAFE stop/start — never k_thread_suspend() the
 * daemon, which could freeze it mid-recv while holding s_chip_lock (deadlock). */
static atomic_t s_rf_daemon_paused = ATOMIC_INIT(0);

static inline void akira_rf_daemon_pause(void)  { atomic_set(&s_rf_daemon_paused, 1); }
static inline void akira_rf_daemon_resume(void) { atomic_set(&s_rf_daemon_paused, 0); }

#define CHIP_LOCK_TIMEOUT_MS 2000

/* RX queue: the daemon fills it; akira_rf_receive() drains it (decoupled from
 * the chip). Shared, so defined here ahead of both users. */
#define RF_RX_MAX_PACKET    255
struct rf_rx_packet { uint8_t data[RF_RX_MAX_PACKET]; uint16_t len; };
#if defined(CONFIG_SPIRAM)
/* Put the large queue data buffer in PSRAM to avoid DRAM overflow on S3 boards */
static uint8_t s_rf_rx_buf[CONFIG_AKIRA_RF_RX_QUEUE_DEPTH * sizeof(struct rf_rx_packet)]
    __attribute__((section(".ext_ram.bss")));
static struct k_msgq s_rf_rx_msgq;
static int s_rf_rx_msgq_inited;
#else
K_MSGQ_DEFINE(s_rf_rx_msgq, sizeof(struct rf_rx_packet),
              CONFIG_AKIRA_RF_RX_QUEUE_DEPTH, 4);
#endif

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

#if defined(CONFIG_SPIRAM)
    if (!s_rf_rx_msgq_inited) {
        k_msgq_init(&s_rf_rx_msgq, (char *)s_rf_rx_buf,
                    sizeof(struct rf_rx_packet), CONFIG_AKIRA_RF_RX_QUEUE_DEPTH);
        s_rf_rx_msgq_inited = 1;
    }
#endif

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

int akira_rf_release_all(void)
{
    LOG_INF("RF release ownership (no power-down)");

    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) {
        return -EBUSY;
    }

    for (int c = AKIRA_RF_CHIP_NONE + 1; c < AKIRA_RF_CHIP_MAX; c++) {
        if (!s_inited[c]) continue;
        radio_handle_t *h = map_chip_to_handle((akira_rf_chip_t)c);
        if (h) {
            radio_manager_release(h, "rf");
        }
    }
    g_active_handle = NULL;
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

int akira_rf_set_bitrate(uint32_t bps)
{
    LOG_INF("RF set bitrate: %u bps", bps);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;
    radio_handle_t *h = g_active_handle;
    int ret = (h && h->ops && h->ops->set_bitrate) ? h->ops->set_bitrate(h, bps) : -ENODEV;
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

#if defined(CONFIG_SPIRAM)
static struct rf_rx_packet s_rf_poll_buf __attribute__((section(".ext_ram.bss")));
#else
static struct rf_rx_packet s_rf_poll_buf;
#endif

static void rf_rx_daemon_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    while (1) {
        if (!g_active_handle) { k_msleep(RF_DAEMON_SLEEP_MS); continue; }
        radio_handle_t *h = g_active_handle;
        if (!h || !h->ops || !h->ops->recv) { k_msleep(RF_DAEMON_SLEEP_MS); continue; }
        if (atomic_get(&s_rf_daemon_paused)) { k_msleep(20); continue; }

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

/* Stateless raw capture: pause the daemon, take the chip, stream into buf.
 * The lock timeout exceeds the daemon's one in-flight 2 s recv so we never
 * spuriously fail to acquire. */
int akira_rf_raw_capture(uint8_t *buf, size_t max_bytes,
                         uint32_t sample_rate_hz, uint32_t timeout_ms)
{
    radio_handle_t *h = g_active_handle;
    if (!h || !h->ops || !h->ops->raw_capture) return -ENOSYS;
    if (!(h->capabilities & RADIO_CAP_RAW_MODE)) return -ENOTSUP;

    akira_rf_daemon_pause();
    int ret = -EBUSY;
    if (k_mutex_lock(&s_chip_lock, K_MSEC(5000)) == 0) {
        ret = h->ops->raw_capture(h, buf, max_bytes, sample_rate_hz, timeout_ms);
        k_mutex_unlock(&s_chip_lock);
    }
    akira_rf_daemon_resume();
    return ret;
}

int akira_rf_raw_replay(const uint8_t *buf, size_t len,
                        uint32_t sample_rate_hz, uint32_t repeat)
{
    radio_handle_t *h = g_active_handle;
    if (!h || !h->ops || !h->ops->raw_replay) return -ENOSYS;
    if (!(h->capabilities & RADIO_CAP_RAW_MODE)) return -ENOTSUP;

    akira_rf_daemon_pause();
    int ret = -EBUSY;
    if (k_mutex_lock(&s_chip_lock, K_MSEC(5000)) == 0) {
        ret = h->ops->raw_replay(h, buf, len, sample_rate_hz, repeat);
        k_mutex_unlock(&s_chip_lock);
    }
    akira_rf_daemon_resume();
    return ret;
}

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

/* =========================================================================
 * Continuous-wave (CW) test tone — for jamming / range testing.
 * Chip must already be selected and frequency+power configured.
 * ========================================================================= */

int akira_rf_tx_cw_start(void)
{
    LOG_INF("RF CW start");
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;

    int ret = -ENODEV;
#if defined(CONFIG_AKIRA_LR2021)
    if (g_active_chip == AKIRA_RF_CHIP_LR2021) {
        ret = lr2021_tx_cw_start();
    }
#endif
    /* Other chips can add their CW implementation here. */

    k_mutex_unlock(&s_chip_lock);
    return ret;
}

int akira_rf_tx_cw_stop(void)
{
    LOG_INF("RF CW stop");
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;

    int ret = -ENODEV;
#if defined(CONFIG_AKIRA_LR2021)
    if (g_active_chip == AKIRA_RF_CHIP_LR2021) {
        ret = lr2021_tx_cw_stop();
    }
#endif

    k_mutex_unlock(&s_chip_lock);
    return ret;
}

/**
 * @brief Fast frequency hop while CW is active — skips CalibFe for speed.
 *
 * Stops CW, sets new frequency (PLL lock only, no calibration),
 * restarts CW.  ~1ms vs ~20ms for a full rf_set_frequency() cycle.
 */
int akira_rf_tx_cw_set_freq(uint32_t freq_hz)
{
    LOG_DBG("RF CW hop: %u Hz", freq_hz);
    if (k_mutex_lock(&s_chip_lock, K_MSEC(CHIP_LOCK_TIMEOUT_MS)) != 0) return -EBUSY;

    int ret = -ENODEV;
#if defined(CONFIG_AKIRA_LR2021)
    if (g_active_chip == AKIRA_RF_CHIP_LR2021) {
        /* Stop CW, fast-set frequency, restart CW in one atomic sequence. */
        lr2021_tx_cw_stop();
        ret = lr2021_tx_cw_set_freq_fast(freq_hz);
        if (ret == 0) {
            ret = lr2021_tx_cw_start();
        }
    }
#endif

    k_mutex_unlock(&s_chip_lock);
    return ret;
}

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

int akira_native_rf_send(wasm_exec_env_t exec_env, void *payload, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (!payload || len == 0) return -EINVAL;

    return akira_rf_send((const uint8_t *)payload, len);
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

int akira_native_rf_set_bitrate(wasm_exec_env_t exec_env, int32_t bps)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    if (bps < 0) return -EINVAL;
    return akira_rf_set_bitrate((uint32_t)bps);
}

/* ── Continuous-wave TX (CW) for jamming / range testing ──────────────── */

int akira_native_rf_tx_cw_start(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    return akira_rf_tx_cw_start();
}

int akira_native_rf_tx_cw_stop(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    return akira_rf_tx_cw_stop();
}

/* Fast frequency hop while CW is active. Type: "(i)i" */
int akira_native_rf_tx_cw_set_freq(wasm_exec_env_t exec_env, uint32_t freq_hz)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
    return akira_rf_tx_cw_set_freq(freq_hz);
}

/* ── Raw Sub-GHz OOK capture / replay ──────────────────────────────────── */

#define RF_RAW_MAX_BYTES 8192u

int akira_native_rf_raw_capture(wasm_exec_env_t exec_env,
                                 uint32_t buf_wasm, uint32_t max_bytes,
                                 uint32_t sample_rate_hz, int32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (max_bytes == 0 || max_bytes > RF_RAW_MAX_BYTES) return -EINVAL;
    if (timeout_ms <= 0) timeout_ms = 5000;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) return -EFAULT;
    uint8_t *buf = (uint8_t *)wasm_runtime_addr_app_to_native(inst, buf_wasm);
    if (!buf || !wasm_runtime_validate_native_addr(inst, buf, max_bytes))
        return -EFAULT;

    return akira_rf_raw_capture(buf, max_bytes, sample_rate_hz,
                                (uint32_t)timeout_ms);
}

int akira_native_rf_raw_replay(wasm_exec_env_t exec_env,
                                uint32_t buf_wasm, uint32_t len,
                                uint32_t sample_rate_hz, int32_t repeat)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);

    if (len == 0 || len > RF_RAW_MAX_BYTES) return -EINVAL;
    if (repeat < 1) repeat = 1;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) return -EFAULT;
    const uint8_t *buf = (const uint8_t *)wasm_runtime_addr_app_to_native(inst, buf_wasm);
    if (!buf || !wasm_runtime_validate_native_addr(inst, (void *)buf, len))
        return -EFAULT;

    return akira_rf_raw_replay(buf, len, sample_rate_hz, (uint32_t)repeat);
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
