/**
 * @file mesh_manager.c
 * @brief AkiraMesh Protocol Manager — orchestration layer
 *
 * Owns radio acquisition and hands it to mesh_mac; wires the MAC, Network
 * (routing abstraction + AODV), Transport, and Application layers together;
 * exposes the public akira_mesh_* API as thin pass-through calls; drives the
 * one shared periodic tick (route GC, RREQ retry, retransmit) that used to
 * be three separate self-rescheduling k_work items before the layered
 * split. All protocol logic lives in mesh_mac.c/mesh_router.c/mesh_aodv.c/
 * mesh_transport.c/mesh_app_dist.c — this file only wires them together.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include "connectivity/akira_mesh.h"
#include "connectivity/radio_interface.h"
#include "connectivity/bluetooth/bt_manager.h"
#include "mesh_mac.h"
#include "mesh_router.h"
#include "mesh_aodv.h"
#include "mesh_transport.h"
#include "mesh_app_dist.h"
#include "mesh_routing.h"
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
#include "mesh_crypto.h"
#endif
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_mesh, CONFIG_AKIRA_LOG_LEVEL);

static struct {
    akira_mesh_config_t config;
    akira_mesh_stats_t  stats;
    radio_handle_t      *radio;
    size_t               mtu;
    uint16_t             seq_num;      /* broadcast DATA only — see akira_mesh_broadcast */
    akira_mesh_rx_cb_t   rx_callback;
    void                *rx_user_data;
    bool                 initialized;
    bool                 started;
} mesh_state;

static K_MUTEX_DEFINE(mesh_init_lock);

static bool is_broadcast(const uint8_t *id)
{
    for (int i = 0; i < AKIRA_MESH_NODE_ID_LEN; i++) {
        if (id[i] != 0xFF) return false;
    }
    return true;
}

/* Test-only link blackhole (see akira_mesh_debug_link_drop) — checked at the
 * single RX dispatch point so a dropped peer is invisible to every layer,
 * same as true radio unreachability. */
#define MESH_DEBUG_LINK_DROP_MAX 4
static uint8_t s_link_drop[MESH_DEBUG_LINK_DROP_MAX][AKIRA_MESH_NODE_ID_LEN];
static bool    s_link_drop_used[MESH_DEBUG_LINK_DROP_MAX];

static bool link_is_dropped(const uint8_t *peer_id)
{
    for (int i = 0; i < MESH_DEBUG_LINK_DROP_MAX; i++) {
        if (s_link_drop_used[i] && memcmp(s_link_drop[i], peer_id, AKIRA_MESH_NODE_ID_LEN) == 0) {
            return true;
        }
    }
    return false;
}

int akira_mesh_debug_link_drop(const uint8_t *peer_id)
{
    if (!peer_id) return -EINVAL;
    if (link_is_dropped(peer_id)) return 0;
    for (int i = 0; i < MESH_DEBUG_LINK_DROP_MAX; i++) {
        if (!s_link_drop_used[i]) {
            memcpy(s_link_drop[i], peer_id, AKIRA_MESH_NODE_ID_LEN);
            s_link_drop_used[i] = true;
            return 0;
        }
    }
    return -ENOMEM;
}

int akira_mesh_debug_link_restore(const uint8_t *peer_id)
{
    if (!peer_id) return -EINVAL;
    for (int i = 0; i < MESH_DEBUG_LINK_DROP_MAX; i++) {
        if (s_link_drop_used[i] && memcmp(s_link_drop[i], peer_id, AKIRA_MESH_NODE_ID_LEN) == 0) {
            s_link_drop_used[i] = false;
            return 0;
        }
    }
    return -ENOENT;
}

bool akira_mesh_debug_link_is_dropped(const uint8_t *peer_id)
{
    return link_is_dropped(peer_id);
}

/* ------------------------------------------------------------------ */
/* Generic RX dispatch — fans frames out to Network/Transport/App.     */
/* ------------------------------------------------------------------ */

static void mesh_rx_from_mac(const uint8_t *buf, size_t len, int16_t rssi, void *ctx)
{
    ARG_UNUSED(ctx);
    if (len < sizeof(struct mesh_header)) return;
    const struct mesh_header *h = (const struct mesh_header *)buf;
    if (h->version != 1) return;
    /* Beacons are genuinely single-hop (never relayed), so src_id here
     * really is the immediate/only sender — unlike RREQ/RREP (filtered on
     * relay_id instead, in mesh_aodv.c) or DATA/STREAM (no per-hop field
     * exists to filter on at all; link-drop can't hop-force those). */
    if (h->msg_type == AKIRA_MESH_MSG_BEACON && link_is_dropped(h->src_id)) return;
    mesh_state.stats.messages_received++;

    switch (h->msg_type) {
    case AKIRA_MESH_MSG_BEACON:
        mesh_aodv_dispatch_beacon(h->src_id, buf + sizeof(*h), len - sizeof(*h), rssi);
        break;
    case AKIRA_MESH_MSG_ROUTE_REQ:
    case AKIRA_MESH_MSG_ROUTE_REPLY:
    case AKIRA_MESH_MSG_ROUTE_ERROR: {
        const mesh_router_ops_t *router = mesh_router_get_active();
        if (router) router->handle_control_frame(buf, len);
        break;
    }
    case AKIRA_MESH_MSG_DATA:
    case AKIRA_MESH_MSG_ACK:
    case AKIRA_MESH_MSG_STREAM_DATA:
    case AKIRA_MESH_MSG_STREAM_STATUS_REQ:
    case AKIRA_MESH_MSG_STREAM_STATUS_RESP:
        mesh_transport_handle_frame(buf, len);
        break;
    case AKIRA_MESH_MSG_APP_START:
    case AKIRA_MESH_MSG_APP_CHUNK:
    case AKIRA_MESH_MSG_APP_STATUS_REQ:
    case AKIRA_MESH_MSG_APP_STATUS_RESP:
        mesh_app_dist_handle_frame(buf, len);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Shared periodic tick — replaces the old three self-rescheduling     */
/* beacon/retransmit/route_gc k_work_delayable items. Beacon scheduling */
/* stays inside mesh_aodv.c's own start/stop (it's routing-specific).   */
/* ------------------------------------------------------------------ */

static void mesh_tick_work_handler(struct k_work *w);
K_WORK_DELAYABLE_DEFINE(mesh_tick_work, mesh_tick_work_handler);

static void mesh_tick_work_handler(struct k_work *w)
{
    ARG_UNUSED(w);
    if (!mesh_state.started) return;
    uint32_t now = k_uptime_get_32();
    const mesh_router_ops_t *router = mesh_router_get_active();
    if (router) router->tick(now);
    mesh_transport_tick(now);
    k_work_schedule(&mesh_tick_work, K_MSEC(CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS));
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int akira_mesh_init(const akira_mesh_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }
    k_mutex_lock(&mesh_init_lock, K_FOREVER);
    if (mesh_state.initialized) {
        k_mutex_unlock(&mesh_init_lock);
        LOG_WRN("AkiraMesh already initialized");
        return -EALREADY;
    }

    memset(&mesh_state.stats, 0, sizeof(mesh_state.stats));
    memcpy(&mesh_state.config, config, sizeof(*config));
    mesh_state.seq_num = 0;

    switch (config->transport) {
        case AKIRA_MESH_TRANSPORT_BLE:
#if defined(CONFIG_BT)
            bt_manager_set_mode(BT_MODE_MESH);
#endif
            mesh_state.radio = radio_manager_acquire_by_type(RADIO_TYPE_BLE, "mesh");
            break;
        case AKIRA_MESH_TRANSPORT_LORA: {
            radio_handle_t *r = radio_manager_get_by_name("LR2021");
            if (r && radio_manager_acquire(r, "mesh") == 0) {
                /* LR2021 boots in FSK for other RF consumers — mesh needs
                 * the actual LoRa PHY, not just "the LR2021 chip". */
                if (radio_set_modulation(r, RADIO_MOD_LORA) != 0) {
                    radio_manager_release(r, "mesh");
                    mesh_state.radio = NULL;
                } else {
                    mesh_state.radio = r;
                }
            } else {
                mesh_state.radio = NULL;
            }
            break;
        }
        case AKIRA_MESH_TRANSPORT_SUBGHZ:
        default: {
            radio_handle_t *r = radio_manager_get_by_name("CC1121");
            mesh_state.radio = (r && radio_manager_acquire(r, "mesh") == 0) ? r : NULL;
            break;
        }
    }
    if (!mesh_state.radio) {
        LOG_ERR("No radio available for mesh transport %d", config->transport);
#if defined(CONFIG_BT)
        if (config->transport == AKIRA_MESH_TRANSPORT_BLE) {
            bt_manager_set_mode(BT_MODE_NONE);
        }
#endif
        k_mutex_unlock(&mesh_init_lock);
        return -ENODEV;
    }

    /* Bring the acquired radio's hardware up — mesh owns it exclusively. */
    if (mesh_state.radio->ops && mesh_state.radio->ops->init) {
        int rret = mesh_state.radio->ops->init(mesh_state.radio);
        if (rret < 0) {
            LOG_ERR("mesh radio '%s' init failed: %d", mesh_state.radio->name, rret);
            radio_manager_release(mesh_state.radio, "mesh");
            mesh_state.radio = NULL;
#if defined(CONFIG_BT)
            if (config->transport == AKIRA_MESH_TRANSPORT_BLE) {
                bt_manager_set_mode(BT_MODE_NONE);
            }
#endif
            k_mutex_unlock(&mesh_init_lock);
            return rret;
        }
    }

    /* MTU for this radio's current config. Radios without get_max_payload()
     * keep the historical MESH_MAC_PACKET_BUF_SIZE bound. */
    mesh_state.mtu = MESH_MAC_PACKET_BUF_SIZE;
    if (mesh_state.radio->ops && mesh_state.radio->ops->get_max_payload) {
        size_t max_payload;
        if (mesh_state.radio->ops->get_max_payload(mesh_state.radio, &max_payload) == 0 &&
            max_payload > sizeof(struct mesh_header)) {
            mesh_state.mtu = MIN(max_payload, MESH_MAC_PACKET_BUF_SIZE);
        }
    }

    mesh_mac_init(mesh_state.radio);
    mesh_mac_register_rx_cb(mesh_rx_from_mac, NULL);

    int aret = mesh_aodv_module_init(config, &mesh_state.stats);
    if (aret) {
        mesh_mac_deinit();
        radio_manager_release(mesh_state.radio, "mesh");
        mesh_state.radio = NULL;
#if defined(CONFIG_BT)
        if (config->transport == AKIRA_MESH_TRANSPORT_BLE) {
            bt_manager_set_mode(BT_MODE_NONE);
        }
#endif
        k_mutex_unlock(&mesh_init_lock);
        return aret;
    }
    mesh_transport_module_init(config, &mesh_state.stats, mesh_state.mtu,
                               &mesh_state.rx_callback, &mesh_state.rx_user_data);
    mesh_app_dist_module_init(config, &mesh_state.stats,
                              &mesh_state.rx_callback, &mesh_state.rx_user_data);

    mesh_state.initialized = true;
    k_mutex_unlock(&mesh_init_lock);
    LOG_INF("AkiraMesh initialized on %s", mesh_state.radio->name);
    return 0;
}

int akira_mesh_start(void)
{
    if (!mesh_state.initialized) {
        return -ENODEV;
    }
    if (mesh_state.started) {
        return 0;
    }
    int rret = mesh_router_acquire("aodv");
    if (rret) {
        LOG_ERR("AkiraMesh: failed to acquire aodv router: %d", rret);
        return rret;
    }
    mesh_state.started = true;
    k_work_schedule(&mesh_tick_work, K_MSEC(CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS));
    LOG_INF("AkiraMesh started");
    return 0;
}

int akira_mesh_stop(void)
{
    k_mutex_lock(&mesh_init_lock, K_FOREVER);
    if (!mesh_state.started && !mesh_state.initialized) {
        k_mutex_unlock(&mesh_init_lock);
        return 0;
    }
    mesh_state.started = false;
    k_work_cancel_delayable(&mesh_tick_work);
    mesh_router_release();
    if (mesh_state.radio) {
        mesh_mac_deinit();
        if (mesh_state.radio->ops && mesh_state.radio->ops->deinit) {
            /* mesh_mac's RX/TX threads only stop polling after this call
             * returns (mesh_mac_deinit() just flips a flag, doesn't join
             * them) — grabbing the same lock they take around send()/recv()
             * ensures deinit's SPI commands never interleave with an
             * in-flight one, which otherwise wedges the chip's BUSY line. */
            k_mutex_lock(&mesh_state.radio->lock, K_FOREVER);
            mesh_state.radio->ops->deinit(mesh_state.radio);
            k_mutex_unlock(&mesh_state.radio->lock);
        }
        radio_manager_release(mesh_state.radio, "mesh");
        mesh_state.radio = NULL;
    }
#if defined(CONFIG_BT)
    if (mesh_state.config.transport == AKIRA_MESH_TRANSPORT_BLE) {
        bt_manager_set_mode(BT_MODE_NONE);
    }
#endif
    mesh_state.initialized = false;
    k_mutex_unlock(&mesh_init_lock);
    LOG_INF("AkiraMesh stopped");
    return 0;
}

int akira_mesh_set_tx_power(int8_t dbm)
{
    if (!mesh_state.radio) {
        return -ENODEV;
    }
    int ret = radio_set_power(mesh_state.radio, dbm);
    if (ret == 0) {
        LOG_INF("AkiraMesh: TX power set to %d dBm", dbm);
    }
    return ret;
}

int akira_mesh_send(const uint8_t *dest_id, const uint8_t *data, size_t len)
{
    if (!mesh_state.initialized || !dest_id || !data) {
        return -EINVAL;
    }
    if (!mesh_state.started) {
        return -ENODEV;
    }

    if (is_broadcast(dest_id)) {
        if (len > mesh_state.mtu - sizeof(struct mesh_header)) {
            return -EMSGSIZE;
        }
        return akira_mesh_broadcast(data, len, mesh_state.config.max_hops);
    }

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    /* Unicast DATA is always encrypted (nonce + tag overhead), unlike
     * broadcast — see MESH_CRYPTO_OVERHEAD. */
    if (mesh_state.mtu < sizeof(struct mesh_header) + MESH_CRYPTO_OVERHEAD ||
        len > mesh_state.mtu - sizeof(struct mesh_header) - MESH_CRYPTO_OVERHEAD) {
        return -EMSGSIZE;
    }
    return mesh_transport_send_reliable(dest_id, AKIRA_MESH_MSG_DATA, data, len);
#else
    /* 1:1 messages are always encrypted (mandatory, not optional) and this
     * build has no crypto module. Never fall back to plaintext DATA. */
    return -ENOTSUP;
#endif
}

int akira_mesh_broadcast(const uint8_t *data, size_t len, uint8_t max_hops)
{
    if (!mesh_state.started) {
        return -ENODEV;
    }
    if (!data) {
        return -EINVAL;
    }
    if (len > MESH_MAC_PACKET_BUF_SIZE - sizeof(struct mesh_header)) {
        return -EMSGSIZE;
    }
    uint8_t bcast[AKIRA_MESH_NODE_ID_LEN];
    memset(bcast, 0xFF, sizeof(bcast));
    uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    h->version = 1;
    h->msg_type = AKIRA_MESH_MSG_DATA;
    h->ttl = max_hops ? max_hops : 1;
    memcpy(h->src_id, mesh_state.config.node_id, AKIRA_MESH_NODE_ID_LEN);
    memcpy(h->dest_id, bcast, AKIRA_MESH_NODE_ID_LEN);
    h->seq_num = mesh_state.seq_num++;
    memcpy(pkt + sizeof(*h), data, len);
    mesh_state.stats.messages_sent++;
    return mesh_mac_send(MESH_MAC_PRIO_LOW, pkt, sizeof(*h) + len);
}

int akira_mesh_get_nodes(akira_mesh_node_info_t *nodes, size_t max_nodes)
{
    if (!nodes || max_nodes == 0) {
        return -EINVAL;
    }
    if (!mesh_state.initialized) {
        return -ENODEV;
    }
    return mesh_aodv_get_nodes(nodes, max_nodes);
}

int akira_mesh_get_stats(akira_mesh_stats_t *stats)
{
    if (!stats) {
        return -EINVAL;
    }
    if (!mesh_state.initialized) {
        return -ENODEV;
    }
    memcpy(stats, &mesh_state.stats, sizeof(akira_mesh_stats_t));
    return 0;
}

int akira_mesh_register_rx_callback(akira_mesh_rx_cb_t callback, void *user_data)
{
    mesh_state.rx_callback = callback;
    mesh_state.rx_user_data = user_data;
    LOG_DBG("Mesh RX callback registered");
    return 0;
}
