/**
 * @file sync_transport.c
 * @brief AkiraSync transport registry and the built-in loopback binding.
 *
 * The registry is deliberately single-slot: a session gossips over exactly one
 * transport, and switching transports mid-session would split the DAG. Binding
 * while a session is open is refused.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include <connectivity/akira_sync.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_sync_transport, CONFIG_AKIRA_LOG_LEVEL);

/* Smallest frame a transport must be able to carry: the event header plus a
 * single byte of payload. Anything smaller cannot gossip at all. */
#define SYNC_MIN_MTU 48

static const struct akira_sync_transport *s_active;
static K_MUTEX_DEFINE(s_lock);

int akira_sync_transport_register(const struct akira_sync_transport *transport)
{
    if (transport) {
        if (!transport->broadcast || !transport->set_rx_cb) {
            return -EINVAL;
        }
        if (transport->mtu < SYNC_MIN_MTU) {
            LOG_ERR("sync: transport '%s' mtu %zu below minimum %d",
                    transport->name ? transport->name : "?", transport->mtu,
                    SYNC_MIN_MTU);
            return -EMSGSIZE;
        }
    }

    k_mutex_lock(&s_lock, K_FOREVER);
    if (akira_sync_is_open()) {
        k_mutex_unlock(&s_lock);
        LOG_ERR("sync: cannot change transport while a session is open");
        return -EBUSY;
    }
    s_active = transport;
    k_mutex_unlock(&s_lock);

    LOG_INF("sync: transport bound to '%s'",
            transport && transport->name ? transport->name : "(none)");
    return 0;
}

const struct akira_sync_transport *akira_sync_transport_active(void)
{
    return s_active;
}

/* ---- Loopback ------------------------------------------------------------
 * Delivers each broadcast straight back to the local node. This is not a
 * degenerate stub: because a node gossips its own events like any other, a
 * single-node session over loopback exercises the identical DAG, ordering and
 * timestamp code paths a multi-node session does. It is what makes the
 * ordering core testable with no network at all.
 */

static akira_sync_rx_cb_t s_lb_cb;
static void              *s_lb_ud;

static int loopback_broadcast(const void *buf, size_t len)
{
    akira_sync_rx_cb_t cb = s_lb_cb;

    if (!cb) {
        return -ENOTCONN;
    }
    /* Delivered synchronously and with a NULL source id: the sync layer reads
     * the creator from the frame itself, never from transport metadata, so
     * loopback and a real network behave identically here. */
    cb(NULL, buf, len, s_lb_ud);
    return 0;
}

static int loopback_set_rx_cb(akira_sync_rx_cb_t cb, void *user_data)
{
    s_lb_cb = cb;
    s_lb_ud = user_data;
    return 0;
}

static const struct akira_sync_transport s_loopback = {
    .name       = "loopback",
    .broadcast  = loopback_broadcast,
    .set_rx_cb  = loopback_set_rx_cb,
    /* Bounded by the event structure rather than by any link layer. */
    .mtu        = AKIRA_SYNC_MAX_PAYLOAD + 64,
};

int akira_sync_transport_bind_loopback(void)
{
    return akira_sync_transport_register(&s_loopback);
}
