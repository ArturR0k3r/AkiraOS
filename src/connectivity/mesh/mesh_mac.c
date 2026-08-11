/**
 * @file mesh_mac.c
 * @brief AkiraMesh MAC layer — CSMA/CA, priority TX queue, RX thread
 *
 * Sole owner of the radio's send/recv calls. Everything above this file
 * enqueues frames instead of touching the radio directly, so a slow
 * transport (BLE's extended-adv send blocks ~240ms) never stalls the caller
 * — only this file's dedicated TX thread pays that cost.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include "mesh_mac.h"
#include "connectivity/akira_mesh.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_mesh_mac, CONFIG_AKIRA_LOG_LEVEL);

struct mac_frame {
    uint16_t len;
    uint8_t  data[MESH_MAC_PACKET_BUF_SIZE];
    int64_t  retry_at;      /* k_uptime_get() ms; not sent before this. 0 = ready now */
    uint8_t  cca_attempts;  /* CCA-busy requeues so far, caps at CCA_MAX_RETRIES */
};

K_MSGQ_DEFINE(s_txq_critical, sizeof(struct mac_frame), CONFIG_AKIRA_MESH_MAC_TXQ_CRITICAL_LEN, 4);
K_MSGQ_DEFINE(s_txq_high, sizeof(struct mac_frame), CONFIG_AKIRA_MESH_MAC_TXQ_HIGH_LEN, 4);
K_MSGQ_DEFINE(s_txq_medium, sizeof(struct mac_frame), CONFIG_AKIRA_MESH_MAC_TXQ_MEDIUM_LEN, 4);
K_MSGQ_DEFINE(s_txq_low, sizeof(struct mac_frame), CONFIG_AKIRA_MESH_MAC_TXQ_LOW_LEN, 4);

/* Priority order: index 0 is polled first every TX thread iteration. */
static struct k_msgq * const s_txq_lanes[] = {
    [MESH_MAC_PRIO_CRITICAL] = &s_txq_critical,
    [MESH_MAC_PRIO_HIGH]     = &s_txq_high,
    [MESH_MAC_PRIO_MEDIUM]   = &s_txq_medium,
    [MESH_MAC_PRIO_LOW]      = &s_txq_low,
};

static struct {
    radio_handle_t   *radio;
    mesh_mac_rx_cb_t  rx_cb;
    void             *rx_ctx;
    bool              running;
} s_mac;

static uint8_t s_rx_buf[MESH_MAC_PACKET_BUF_SIZE]
#if defined(CONFIG_SPIRAM)
  __attribute__((section(".ext_ram.bss")))
#endif
;

int mesh_mac_init(radio_handle_t *radio)
{
    if (!radio) return -EINVAL;
    s_mac.radio = radio;
    s_mac.running = true;
    return 0;
}

int mesh_mac_deinit(void)
{
    s_mac.running = false;
    s_mac.radio = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(s_txq_lanes); i++) {
        k_msgq_purge(s_txq_lanes[i]);
    }
    return 0;
}

int mesh_mac_register_rx_cb(mesh_mac_rx_cb_t cb, void *ctx)
{
    s_mac.rx_cb = cb;
    s_mac.rx_ctx = ctx;
    return 0;
}

int mesh_mac_send(mesh_mac_prio_t prio, const uint8_t *buf, size_t len)
{
    if (!s_mac.running || !s_mac.radio) return -ENODEV;
    if (len > MESH_MAC_PACKET_BUF_SIZE) return -EMSGSIZE;
    if ((unsigned)prio >= ARRAY_SIZE(s_txq_lanes)) return -EINVAL;
    struct mac_frame f = { .len = (uint16_t)len, .retry_at = 0, .cca_attempts = 0 };
    memcpy(f.data, buf, len);
    return (k_msgq_put(s_txq_lanes[prio], &f, K_NO_WAIT) == 0) ? 0 : -EBUSY;
}

mesh_mac_prio_t mesh_mac_prio_for_msg_type(uint8_t msg_type)
{
    switch (msg_type) {
    case AKIRA_MESH_MSG_BEACON:
    case AKIRA_MESH_MSG_ROUTE_REQ:
    case AKIRA_MESH_MSG_ROUTE_REPLY:
    case AKIRA_MESH_MSG_ROUTE_ERROR:
    case AKIRA_MESH_MSG_ROUTE_REQ_SIG:
    case AKIRA_MESH_MSG_ROUTE_REPLY_SIG:
        return MESH_MAC_PRIO_CRITICAL;
    case AKIRA_MESH_MSG_ACK:
        return MESH_MAC_PRIO_HIGH;
    case AKIRA_MESH_MSG_APP_STATUS_REQ:
    case AKIRA_MESH_MSG_APP_STATUS_RESP:
    case AKIRA_MESH_MSG_STREAM_STATUS_REQ:
    case AKIRA_MESH_MSG_STREAM_STATUS_RESP:
        return MESH_MAC_PRIO_MEDIUM;
    default: /* DATA, APP_CHUNK, APP_START, STREAM_DATA */
        return MESH_MAC_PRIO_LOW;
    }
}

/* CCA: only meaningful on radios that advertise it (CC1121/LR2021 today;
 * BLE's extended-adv send doesn't set RADIO_CAP_CCA, so this is skipped for
 * it automatically — no special-casing needed). Single non-blocking check —
 * the TX thread requeues on busy instead of sleeping here, so a lower-lane
 * frame's backoff never blocks a higher-priority frame arriving meanwhile. */
static bool mesh_cca_busy(void)
{
    if (!radio_has_capability(s_mac.radio, RADIO_CAP_CCA)) return false;
    int16_t rssi;
    if (radio_get_rssi(s_mac.radio, &rssi) != 0) return false; /* can't sense, just send */
    return rssi >= CONFIG_AKIRA_MESH_CCA_BUSY_RSSI_DBM;
}

static void mesh_mac_tx_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (1) {
        if (!s_mac.running || !s_mac.radio || !s_mac.radio->ops || !s_mac.radio->ops->send) {
            k_msleep(CONFIG_AKIRA_MESH_MAC_POLL_MS);
            continue;
        }

        struct mac_frame f;
        struct k_msgq *from = NULL;
        for (size_t i = 0; i < ARRAY_SIZE(s_txq_lanes); i++) {
            if (k_msgq_get(s_txq_lanes[i], &f, K_NO_WAIT) == 0) {
                from = s_txq_lanes[i];
                break;
            }
        }
        if (!from) {
            k_msleep(CONFIG_AKIRA_MESH_MAC_POLL_MS);
            continue;
        }

        int64_t now = k_uptime_get();
        if (f.retry_at && now < f.retry_at) {
            if (k_msgq_put(from, &f, K_NO_WAIT) != 0) {
                LOG_ERR("mesh_mac: frame dropped, lane full re-queuing backoff wait");
            }
            k_msleep(CONFIG_AKIRA_MESH_MAC_POLL_MS);
            continue;
        }

        /* CCA never blocks a send indefinitely: after CCA_MAX_RETRIES busy
         * requeues it sends anyway, since CCA reduces collisions but the
         * ACK/retry layer above is the actual reliability backstop. */
        if (f.cca_attempts < CONFIG_AKIRA_MESH_CCA_MAX_RETRIES && mesh_cca_busy()) {
            f.cca_attempts++;
            f.retry_at = now + (sys_rand32_get() % (CONFIG_AKIRA_MESH_CCA_BACKOFF_MAX_MS + 1));
            if (k_msgq_put(from, &f, K_NO_WAIT) != 0) {
                LOG_ERR("mesh_mac: frame dropped, lane full re-queuing CCA backoff");
            }
            continue; /* re-poll from top immediately — higher lanes get serviced now */
        }

        int lret = k_mutex_lock(&s_mac.radio->lock, K_MSEC(2000));
        if (lret != 0) continue;
        int ret = s_mac.radio->ops->send(s_mac.radio, f.data, f.len);
        k_mutex_unlock(&s_mac.radio->lock);
        if (ret) {
            LOG_ERR("mesh_mac send failed: %d (len=%u)", ret, f.len);
        }
    }
}

K_THREAD_DEFINE(mesh_mac_tx_thread, CONFIG_AKIRA_MESH_RX_STACK_SIZE,
                mesh_mac_tx_thread_fn, NULL, NULL, NULL,
                CONFIG_AKIRA_MESH_RX_PRIORITY, 0, 0);

static void mesh_mac_rx_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (1) {
        if (!s_mac.running || !s_mac.radio) {
            k_msleep(CONFIG_AKIRA_MESH_RX_POLL_MS);
            continue;
        }
        radio_handle_t *r = s_mac.radio;
        int n = -ENODEV;
        int16_t rssi = 0;
        if (r->ops && r->ops->recv) {
            if (k_mutex_lock(&r->lock, K_MSEC(2000)) == 0) {
                /* Short window: chip stays in continuous RX between cycles,
                 * so ISR fires immediately on packet arrival regardless.
                 * Keeping the window short lets beacon/TX grab the bus. */
                n = r->ops->recv(r, s_rx_buf, sizeof(s_rx_buf), 500);
                /* Same lock as recv() — this is another SPI transaction on
                 * the same chip, and radio->lock is what keeps MAC-layer
                 * traffic from interleaving with itself (see mesh_stop's
                 * deinit fix for what happens when that's skipped). Best
                 * effort: a failure here just leaves rssi at 0 ("unknown"),
                 * it doesn't invalidate the frame that was received. */
                if (n > 0) radio_get_last_rx_rssi(r, &rssi);
                k_mutex_unlock(&r->lock);
            }
        }
        if (n > 0) {
            LOG_INF("mesh_mac RX: %d bytes, rssi=%d dBm", n, rssi);
            if (s_mac.rx_cb) {
                s_mac.rx_cb(s_rx_buf, (size_t)n, rssi, s_mac.rx_ctx);
            }
        } else {
            /* The IRQ-driven recv path returns almost instantly regardless
             * of whether a packet arrived — with no sleep here this becomes
             * a tight loop that re-acquires the shared SPI bus lock
             * continuously, starving other bus users (e.g. an SD card). */
            k_msleep(CONFIG_AKIRA_MESH_RX_IDLE_YIELD_MS);
        }
    }
}

K_THREAD_DEFINE(mesh_mac_rx_thread, CONFIG_AKIRA_MESH_RX_STACK_SIZE,
                mesh_mac_rx_thread_fn, NULL, NULL, NULL,
                CONFIG_AKIRA_MESH_RX_PRIORITY, 0, 0);
