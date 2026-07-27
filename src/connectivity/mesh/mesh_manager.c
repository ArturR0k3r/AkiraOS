/**
 * @file mesh_manager.c
 * @brief AkiraMesh Protocol Manager Implementation
 *
 * Owns radio acquisition, the RX thread, RX dispatch, AODV route wiring, and
 * end-to-end reliable delivery. All pure table logic lives in mesh_routing.c.
 *
 * Locking discipline (two locks, never deadlock):
 *   - radio->lock  : the hardware bus. Held ONLY around ops->send / ops->recv
 *                    (inside mesh_radio_tx and the RX thread). Never across
 *                    dispatch.
 *   - tables_lock  : guards all software tables (seen/route/ack/pending) and the
 *                    node table. When building a packet from table data, take
 *                    tables_lock, copy what is needed, RELEASE it, THEN call
 *                    mesh_radio_tx (which takes the bus lock internally).
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include "connectivity/akira_mesh.h"
#include "connectivity/radio_interface.h"
#include "connectivity/bluetooth/bt_manager.h"
#include "mesh_routing.h"
#include "mesh_crypto.h"
#include "mesh_session.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <lib/mem_helper.h>

LOG_MODULE_REGISTER(akira_mesh, CONFIG_AKIRA_LOG_LEVEL);

#define MESH_PACKET_BUF_SIZE 256
#define MESH_BEACON_BUF_SIZE 64

#define MESH_ROUTE_LIFETIME_MS (CONFIG_AKIRA_MESH_ROUTE_LIFETIME_S * 1000U)

/* Mesh protocol header */
struct __packed mesh_header {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  ttl;
    uint8_t  src_id[AKIRA_MESH_NODE_ID_LEN];
    uint8_t  dest_id[AKIRA_MESH_NODE_ID_LEN];
    uint16_t seq_num;
};

/* AODV control payloads (follow a mesh_header) */
/* orig_identity_pub / target_prekey_pub piggyback an async X3DH-lite E2E
 * handshake on route discovery: RREQ carries the originator's long-term
 * identity pubkey, RREP carries the target's current ephemeral prekey
 * pubkey. Both sides derive the same session key via
 * ECDH(identity_priv, peer_prekey_pub) === ECDH(prekey_priv, peer_identity_pub)
 * without needing to be online simultaneously — see mesh_dispatch's
 * ROUTE_REQ/ROUTE_REPLY handling. Zero-filled when E2E crypto isn't built. */
struct __packed aodv_rreq {
    uint8_t  target[AKIRA_MESH_NODE_ID_LEN];
    uint16_t rreq_id;
    uint16_t orig_seq;
    uint16_t dest_seq;
    uint8_t  orig_identity_pub[MESH_CRYPTO_PUB_LEN];
};
struct __packed aodv_rrep {
    uint8_t  target[AKIRA_MESH_NODE_ID_LEN];
    uint16_t dest_seq;
    uint8_t  hop_count;
    uint8_t  target_prekey_pub[MESH_CRYPTO_PUB_LEN];
};
struct __packed aodv_rerr {
    uint8_t  unreachable[AKIRA_MESH_NODE_ID_LEN];
    uint16_t dest_seq;
};

/* Mesh manager state */
static struct {
    akira_mesh_config_t    config;
    akira_mesh_stats_t     stats;
    radio_handle_t        *radio;
    akira_mesh_rx_cb_t     rx_callback;
    void                  *rx_user_data;
    akira_mesh_node_info_t nodes[AKIRA_MESH_MAX_NODES];
    uint8_t                node_count;
    uint16_t               seq_num;     /* per-packet sequence */
    uint16_t               aodv_seq;    /* AODV freshness sequence */
    struct seen_cache      seen;
    struct route_table     routes;
    struct ack_table       acks;
    struct pending_route_q proutes;
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    struct session_table   sessions;    /* E2E session keys, guarded by tables_lock */
    uint8_t                identity_priv[MESH_CRYPTO_PRIV_LEN]; /* long-term P-256 id */
    uint8_t                identity_pub[MESH_CRYPTO_PUB_LEN];
    uint8_t                prekey_priv[MESH_CRYPTO_PRIV_LEN];   /* ephemeral, RREP-carried */
    uint8_t                prekey_pub[MESH_CRYPTO_PUB_LEN];
#endif
    size_t                  mtu;         /* radio's current max payload, header included */
    struct k_mutex         tables_lock; /* guards tables + node table + counters */
    bool initialized;
    bool started;
} mesh_state AKIRA_BULK_BSS; /* ~7 KB routing/node tables — PSRAM on boards that
                              * have it (thread-only, mutex-guarded, never ISR).
                              * No-op on non-PSRAM targets. */

static K_MUTEX_DEFINE(mesh_init_lock);   /* serialize init/stop */

/* Forward declarations */
static void mesh_dispatch(const uint8_t *buf, size_t len);
static void update_neighbor(const uint8_t *src_id, const uint8_t *payload, size_t plen);
static void send_rerr(const uint8_t *to_immediate, const uint8_t *unreachable);
static void send_rreq(const uint8_t *target);

static void beacon_work_handler(struct k_work *w);
static void retransmit_work_handler(struct k_work *w);
static void route_gc_work_handler(struct k_work *w);

/* Work-item handles declared before akira_mesh_stop references them. */
K_WORK_DELAYABLE_DEFINE(beacon_work,     beacon_work_handler);
K_WORK_DELAYABLE_DEFINE(retransmit_work, retransmit_work_handler);
K_WORK_DELAYABLE_DEFINE(route_gc_work,   route_gc_work_handler);

/* ------------------------------------------------------------------ */
/* Bus-lock TX helper — radio->lock held ONLY around ops->send.        */
/* ------------------------------------------------------------------ */

static int mesh_radio_tx(const uint8_t *buf, size_t len)
{
    radio_handle_t *r = mesh_state.radio;
    if (!r || !r->ops || !r->ops->send) {
        return -ENODEV;
    }
    int lret = k_mutex_lock(&r->lock, K_MSEC(2000));
    if (lret != 0) {
        return -EBUSY;
    }
    int ret = r->ops->send(r, buf, len);
    k_mutex_unlock(&r->lock);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static bool is_self(const uint8_t *id)
{
    return memcmp(id, mesh_state.config.node_id, AKIRA_MESH_NODE_ID_LEN) == 0;
}

static bool is_broadcast(const uint8_t *id)
{
    for (int i = 0; i < AKIRA_MESH_NODE_ID_LEN; i++) {
        if (id[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static void fill_header(struct mesh_header *h, uint8_t type, uint8_t ttl, const uint8_t *dest)
{
    h->version = 1;
    h->msg_type = type;
    h->ttl = ttl;
    memcpy(h->src_id, mesh_state.config.node_id, AKIRA_MESH_NODE_ID_LEN);
    memcpy(h->dest_id, dest, AKIRA_MESH_NODE_ID_LEN);
    h->seq_num = mesh_state.seq_num++;
}

/* ------------------------------------------------------------------ */
/* AODV / control-frame TX builders (each builds buffer then TX).      */
/* No tables_lock held across mesh_radio_tx.                           */
/* ------------------------------------------------------------------ */

static void send_rrep(const uint8_t *to_immediate, const uint8_t *target,
                      uint16_t dest_seq, uint8_t hop_count)
{
    uint8_t pkt[sizeof(struct mesh_header) + sizeof(struct aodv_rrep)];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_header(h, AKIRA_MESH_MSG_ROUTE_REPLY, mesh_state.config.max_hops, to_immediate);
    struct aodv_rrep *rp = (struct aodv_rrep *)(pkt + sizeof(*h));
    memcpy(rp->target, target, AKIRA_MESH_NODE_ID_LEN);
    rp->dest_seq = dest_seq;
    rp->hop_count = hop_count;
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    memcpy(rp->target_prekey_pub, mesh_state.prekey_pub, MESH_CRYPTO_PUB_LEN);
#else
    memset(rp->target_prekey_pub, 0, MESH_CRYPTO_PUB_LEN);
#endif
    mesh_radio_tx(pkt, sizeof(pkt));
}

static void send_ack(const uint8_t *to, uint16_t seq)
{
    uint8_t pkt[sizeof(struct mesh_header)];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_header(h, AKIRA_MESH_MSG_ACK, 1, to);
    h->seq_num = seq;   /* echo the acked seq */
    mesh_radio_tx(pkt, sizeof(pkt));
}

static void send_rreq(const uint8_t *target)
{
    uint8_t bcast[AKIRA_MESH_NODE_ID_LEN];
    memset(bcast, 0xFF, sizeof(bcast));

    uint8_t pkt[sizeof(struct mesh_header) + sizeof(struct aodv_rreq)];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_header(h, AKIRA_MESH_MSG_ROUTE_REQ, mesh_state.config.max_hops, bcast);
    struct aodv_rreq *rq = (struct aodv_rreq *)(pkt + sizeof(*h));
    memcpy(rq->target, target, AKIRA_MESH_NODE_ID_LEN);
    rq->rreq_id = h->seq_num;
    rq->orig_seq = ++mesh_state.aodv_seq;
    rq->dest_seq = 0;
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    memcpy(rq->orig_identity_pub, mesh_state.identity_pub, MESH_CRYPTO_PUB_LEN);
#else
    memset(rq->orig_identity_pub, 0, MESH_CRYPTO_PUB_LEN);
#endif
    mesh_radio_tx(pkt, sizeof(pkt));
}

static void send_rerr(const uint8_t *to_immediate, const uint8_t *unreachable)
{
    uint8_t pkt[sizeof(struct mesh_header) + sizeof(struct aodv_rerr)];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_header(h, AKIRA_MESH_MSG_ROUTE_ERROR, mesh_state.config.max_hops, to_immediate);
    struct aodv_rerr *re = (struct aodv_rerr *)(pkt + sizeof(*h));
    memcpy(re->unreachable, unreachable, AKIRA_MESH_NODE_ID_LEN);
    re->dest_seq = 0;
    mesh_radio_tx(pkt, sizeof(pkt));
}

/* ------------------------------------------------------------------ */
/* Forwarding + pending-route flush                                    */
/* ------------------------------------------------------------------ */

static void forward_data(struct mesh_header *h, const uint8_t *full, size_t len)
{
    if (h->ttl == 0) {
        return;
    }
    /* Look up route under tables_lock, copy next-hop, release before TX. */
    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    struct route_entry *r = mesh_route_lookup(&mesh_state.routes, h->dest_id,
                                              k_uptime_get_32());
    bool have = (r != NULL);
    k_mutex_unlock(&mesh_state.tables_lock);

    if (!have) {
        send_rerr(h->src_id, h->dest_id);
        return;
    }
    uint8_t pkt[MESH_PACKET_BUF_SIZE];
    if (len > sizeof(pkt)) {
        return;
    }
    memcpy(pkt, full, len);
    ((struct mesh_header *)pkt)->ttl = h->ttl - 1;
    mesh_state.stats.messages_forwarded++;
    mesh_radio_tx(pkt, len);
}

static void flush_pending_for(const uint8_t *dest)
{
    while (1) {
        /* Copy one queued payload out under the lock, release, then send.
         * akira_mesh_send re-takes tables_lock — so we must NOT hold it here. */
        k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
        struct pending_route *e = mesh_pr_next_for_dest(&mesh_state.proutes, dest);
        if (!e) {
            k_mutex_unlock(&mesh_state.tables_lock);
            break;
        }
        uint8_t buf[MESH_PACKET_BUF_SIZE];
        uint16_t l = e->len;
        if (l > sizeof(buf)) {
            l = sizeof(buf);
        }
        memcpy(buf, e->payload, l);
        mesh_pr_clear_slot(e);
        k_mutex_unlock(&mesh_state.tables_lock);

        /* The queued payload is a full DATA packet (header + data); re-send the
         * application payload through the normal send path now a route exists. */
        akira_mesh_send(dest, buf + sizeof(struct mesh_header),
                        l - sizeof(struct mesh_header));
    }
}

/* ------------------------------------------------------------------ */
/* Neighbor (node) table update — ported from the old beacon handler.  */
/* Guarded by tables_lock.                                             */
/* ------------------------------------------------------------------ */

static void update_neighbor(const uint8_t *src_id, const uint8_t *payload, size_t plen)
{
    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);

    for (uint8_t i = 0; i < mesh_state.node_count; i++) {
        if (memcmp(mesh_state.nodes[i].node_id, src_id, AKIRA_MESH_NODE_ID_LEN) == 0) {
            mesh_state.nodes[i].last_seen = k_uptime_get_32();
            mesh_state.nodes[i].rssi = 0;
            mesh_state.nodes[i].lqi = 0;
            /* Re-fill name if first beacon arrived before name was set. */
            if (mesh_state.nodes[i].name[0] == '\0' && plen > 0 &&
                plen < sizeof(mesh_state.nodes[i].name)) {
                memcpy(mesh_state.nodes[i].name, payload, plen);
                mesh_state.nodes[i].name[plen] = '\0';
                LOG_INF("Updated name for known node: %s", mesh_state.nodes[i].name);
            }
            k_mutex_unlock(&mesh_state.tables_lock);
            return;
        }
    }

    if (mesh_state.node_count < AKIRA_MESH_MAX_NODES) {
        akira_mesh_node_info_t *node = &mesh_state.nodes[mesh_state.node_count++];
        memset(node, 0, sizeof(*node));
        memcpy(node->node_id, src_id, AKIRA_MESH_NODE_ID_LEN);

        /* Beacon payload carries the node name. */
        if (plen > 0 && plen < sizeof(node->name)) {
            memcpy(node->name, payload, plen);
            node->name[plen] = '\0';
        }
        node->hop_count = 1;
        /* Per-packet RSSI/LQI unavailable via ops->recv; left 0 (follow-up). */
        node->rssi = 0;
        node->lqi = 0;
        node->last_seen = k_uptime_get_32();
        node->role = AKIRA_MESH_ROLE_NODE;

        mesh_state.stats.nodes_discovered++;
        LOG_INF("Discovered mesh node: %s", node->name);
    }

    k_mutex_unlock(&mesh_state.tables_lock);
}

/* ------------------------------------------------------------------ */
/* RX dispatch — runs in the RX thread with NO lock held on entry.     */
/* ------------------------------------------------------------------ */

static void mesh_dispatch(const uint8_t *buf, size_t len)
{
    if (len < sizeof(struct mesh_header)) {
        return;
    }
    const struct mesh_header *h = (const struct mesh_header *)buf;
    if (h->version != 1) {
        return;
    }
    mesh_state.stats.messages_received++;

    /* Duplicate suppression — skip for unicast ACK (echoes acked seq). */
    if (h->msg_type != AKIRA_MESH_MSG_ACK) {
        k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
        bool dup = mesh_seen_check_and_add(&mesh_state.seen, h->src_id, h->seq_num);
        k_mutex_unlock(&mesh_state.tables_lock);
        if (dup) {
            return;
        }
    }

    const uint8_t *payload = buf + sizeof(*h);
    size_t plen = len - sizeof(*h);
    uint32_t now = k_uptime_get_32();

    switch (h->msg_type) {
    case AKIRA_MESH_MSG_BEACON:
        update_neighbor(h->src_id, payload, plen);
        break;

    case AKIRA_MESH_MSG_ROUTE_REQ: {
        if (plen < sizeof(struct aodv_rreq)) {
            return;
        }
        const struct aodv_rreq *rq = (const struct aodv_rreq *)payload;
        k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
        mesh_route_install(&mesh_state.routes, h->src_id, h->src_id, 1,
                           rq->orig_seq, now + MESH_ROUTE_LIFETIME_MS);
        k_mutex_unlock(&mesh_state.tables_lock);
        if (is_self(rq->target)) {
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
            uint8_t shared[MESH_CRYPTO_SHARED_LEN];
            if (mesh_crypto_p256_ecdh(mesh_state.prekey_priv, rq->orig_identity_pub,
                                      shared) == 0) {
                k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
                mesh_session_install(&mesh_state.sessions, mesh_state.config.node_id,
                                     h->src_id, shared, sizeof(shared), now,
                                     CONFIG_AKIRA_MESH_SESSION_LIFETIME_S * 1000U);
                k_mutex_unlock(&mesh_state.tables_lock);
            } else {
                LOG_WRN("AkiraMesh: ECDH failed deriving session for RREQ from %02x%02x",
                        h->src_id[0], h->src_id[1]);
            }
            memset(shared, 0, sizeof(shared));
#endif
            send_rrep(h->src_id, mesh_state.config.node_id, mesh_state.aodv_seq, 0);
        } else if (h->ttl > 1) {
            uint8_t pkt[MESH_PACKET_BUF_SIZE];
            if (len <= sizeof(pkt)) {
                memcpy(pkt, buf, len);
                ((struct mesh_header *)pkt)->ttl = h->ttl - 1;
                mesh_radio_tx(pkt, len);
            }
        }
        break;
    }

    case AKIRA_MESH_MSG_ROUTE_REPLY: {
        if (plen < sizeof(struct aodv_rrep)) {
            return;
        }
        const struct aodv_rrep *rp = (const struct aodv_rrep *)payload;
        k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
        mesh_route_install(&mesh_state.routes, rp->target, h->src_id,
                           rp->hop_count + 1, rp->dest_seq,
                           now + MESH_ROUTE_LIFETIME_MS);
        k_mutex_unlock(&mesh_state.tables_lock);
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
        if (is_self(h->dest_id)) {
            uint8_t shared[MESH_CRYPTO_SHARED_LEN];
            if (mesh_crypto_p256_ecdh(mesh_state.identity_priv, rp->target_prekey_pub,
                                      shared) == 0) {
                k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
                mesh_session_install(&mesh_state.sessions, mesh_state.config.node_id,
                                     rp->target, shared, sizeof(shared), now,
                                     CONFIG_AKIRA_MESH_SESSION_LIFETIME_S * 1000U);
                k_mutex_unlock(&mesh_state.tables_lock);
            } else {
                LOG_WRN("AkiraMesh: ECDH failed deriving session for RREP from %02x%02x",
                        rp->target[0], rp->target[1]);
            }
            memset(shared, 0, sizeof(shared));
        }
#endif
        flush_pending_for(rp->target);
        if (!is_self(h->dest_id) && h->ttl > 1) {
            /* Forward RREP toward the originator via the reverse route. */
            forward_data((struct mesh_header *)buf, buf, len);
        }
        break;
    }

    case AKIRA_MESH_MSG_ROUTE_ERROR: {
        if (plen < sizeof(struct aodv_rerr)) {
            return;
        }
        const struct aodv_rerr *re = (const struct aodv_rerr *)payload;
        k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
        mesh_route_invalidate(&mesh_state.routes, re->unreachable);
        k_mutex_unlock(&mesh_state.tables_lock);
        break;
    }

    case AKIRA_MESH_MSG_DATA:
        if (is_self(h->dest_id)) {
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
            if (plen < MESH_CRYPTO_NONCE_LEN + MESH_CRYPTO_MAC_LEN) {
                break; /* too short to be a valid encrypted DATA frame */
            }
            const uint8_t *nonce = payload;
            const uint8_t *ct = payload + MESH_CRYPTO_NONCE_LEN;
            size_t ct_len = plen - MESH_CRYPTO_NONCE_LEN - MESH_CRYPTO_MAC_LEN;
            const uint8_t *tag = ct + ct_len;

            k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
            struct session_entry *sess =
                mesh_session_lookup(&mesh_state.sessions, h->src_id, now);
            uint8_t enc_key[MESH_CRYPTO_SESSION_KEY_LEN];
            uint8_t mac_key[MESH_CRYPTO_MAC_KEY_LEN];
            bool have_sess = (sess != NULL);
            if (have_sess) {
                memcpy(enc_key, sess->enc_key, sizeof(enc_key));
                memcpy(mac_key, sess->mac_key, sizeof(mac_key));
            }
            k_mutex_unlock(&mesh_state.tables_lock);

            if (!have_sess) {
                LOG_WRN("AkiraMesh: DATA from %02x%02x with no cached session, dropped",
                        h->src_id[0], h->src_id[1]);
                break;
            }

            uint8_t expect_tag[32];
            mesh_crypto_hmac_sha256(mac_key, sizeof(mac_key), payload,
                                    MESH_CRYPTO_NONCE_LEN + ct_len, expect_tag);
            bool tag_ok = mesh_crypto_const_time_eq(expect_tag, tag, MESH_CRYPTO_MAC_LEN);
            if (!tag_ok) {
                memset(enc_key, 0, sizeof(enc_key));
                memset(mac_key, 0, sizeof(mac_key));
                memset(expect_tag, 0, sizeof(expect_tag));
                LOG_WRN("AkiraMesh: DATA MAC mismatch from %02x%02x, dropped",
                        h->src_id[0], h->src_id[1]);
                break;
            }

            uint8_t plaintext[MESH_PACKET_BUF_SIZE];
            mesh_crypto_aes256_ctr(enc_key, nonce, ct, ct_len, plaintext);
            memset(enc_key, 0, sizeof(enc_key));
            memset(mac_key, 0, sizeof(mac_key));
            memset(expect_tag, 0, sizeof(expect_tag));

            send_ack(h->src_id, h->seq_num);
            if (mesh_state.rx_callback) {
                mesh_state.rx_callback(h->src_id, plaintext, ct_len, mesh_state.rx_user_data);
            }
            memset(plaintext, 0, sizeof(plaintext));
#else
            /* No crypto module in this build — 1:1 DATA can't be verified
             * or decrypted. Drop rather than deliver as if it were
             * plaintext (mandatory E2E, no fallback — see akira_mesh_send). */
            LOG_WRN("AkiraMesh: DATA received but E2E crypto not compiled in, dropped");
#endif
        } else if (is_broadcast(h->dest_id)) {
            if (mesh_state.rx_callback) {
                mesh_state.rx_callback(h->src_id, payload, plen,
                                       mesh_state.rx_user_data);
            }
            if (h->ttl > 1) {
                uint8_t pkt[MESH_PACKET_BUF_SIZE];
                if (len <= sizeof(pkt)) {
                    memcpy(pkt, buf, len);
                    ((struct mesh_header *)pkt)->ttl = h->ttl - 1;
                    mesh_state.stats.messages_forwarded++;
                    mesh_radio_tx(pkt, len);
                }
            }
        } else {
            forward_data((struct mesh_header *)buf, buf, len);
        }
        break;

    case AKIRA_MESH_MSG_ACK:
        k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
        mesh_ack_clear(&mesh_state.acks, h->seq_num, h->src_id);
        k_mutex_unlock(&mesh_state.tables_lock);
        break;

    default:
        LOG_DBG("Unhandled mesh message type: %d", h->msg_type);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* RX thread — polls the radio under the bus lock, dispatches without. */
/* ------------------------------------------------------------------ */

static uint8_t s_rx_buf[MESH_PACKET_BUF_SIZE];

static void mesh_rx_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (1) {
        if (!mesh_state.started || !mesh_state.radio) {
            k_msleep(CONFIG_AKIRA_MESH_RX_POLL_MS);
            continue;
        }
        radio_handle_t *r = mesh_state.radio;
        int n = -ENODEV;
        if (r->ops && r->ops->recv) {
            if (k_mutex_lock(&r->lock, K_MSEC(2000)) == 0) {
                /* Short window: chip stays in continuous RX between cycles,
                 * so ISR fires immediately on packet arrival regardless.
                 * Keeping the window short lets beacon/TX grab the bus. */
                n = r->ops->recv(r, s_rx_buf, sizeof(s_rx_buf), 500);
                k_mutex_unlock(&r->lock);
            }
        }
        if (n > 0) {
            mesh_dispatch(s_rx_buf, (size_t)n);   /* bus lock NOT held */
        }
    }
}

K_THREAD_DEFINE(mesh_rx_thread, CONFIG_AKIRA_MESH_RX_STACK_SIZE,
                mesh_rx_thread_fn, NULL, NULL, NULL,
                CONFIG_AKIRA_MESH_RX_PRIORITY, 0, 0);

/* ------------------------------------------------------------------ */
/* Work items — beacon / retransmit / route GC                         */
/* ------------------------------------------------------------------ */

/* ±20% jitter so two nodes started together don't stay beacon-phase-locked:
 * with a fixed period, nodes whose beacon windows align on a half-duplex
 * radio never break the tie and never hear each other. */
static uint32_t beacon_next_delay_ms(void)
{
    int32_t range = (int32_t)mesh_state.config.beacon_interval_ms / 5;
    int32_t jitter = (int32_t)(sys_rand32_get() % (2 * range + 1)) - range;
    return mesh_state.config.beacon_interval_ms + jitter;
}

static void beacon_work_handler(struct k_work *w)
{
    ARG_UNUSED(w);
    if (!mesh_state.started) {
        return;
    }
    uint8_t bcast[AKIRA_MESH_NODE_ID_LEN];
    memset(bcast, 0xFF, sizeof(bcast));
    uint8_t pkt[MESH_BEACON_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_header(h, AKIRA_MESH_MSG_BEACON, 1, bcast);
    size_t nlen = strlen(mesh_state.config.node_name);
    if (sizeof(*h) + nlen > sizeof(pkt)) {
        nlen = sizeof(pkt) - sizeof(*h);
    }
    memcpy(pkt + sizeof(*h), mesh_state.config.node_name, nlen);
    LOG_INF("beacon TX: name='%s' nlen=%zu seq=%u", mesh_state.config.node_name, nlen, h->seq_num);
    mesh_radio_tx(pkt, sizeof(*h) + nlen);
    mesh_state.stats.messages_sent++;
    k_work_schedule(&beacon_work, K_MSEC(beacon_next_delay_ms()));
}

static void retransmit_work_handler(struct k_work *w)
{
    ARG_UNUSED(w);
    if (!mesh_state.started) {
        return;
    }
    uint32_t now = k_uptime_get_32();
    for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_PENDING_ACKS; i++) {
        uint8_t buf[MESH_PACKET_BUF_SIZE];
        uint16_t blen = 0;

        k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
        struct pending_ack *e = &mesh_state.acks.e[i];
        mesh_ack_action_t act = mesh_ack_tick(e, now, CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS);
        if (act == MESH_ACK_RETRANSMIT) {
            blen = e->len;
            if (blen > sizeof(buf)) {
                blen = sizeof(buf);
            }
            memcpy(buf, e->payload, blen);
        } else if (act == MESH_ACK_GIVE_UP) {
            uint8_t dest[AKIRA_MESH_NODE_ID_LEN];
            memcpy(dest, e->dest_id, AKIRA_MESH_NODE_ID_LEN);
            e->active = false;
            mesh_route_invalidate(&mesh_state.routes, dest);
        }
        k_mutex_unlock(&mesh_state.tables_lock);

        if (act == MESH_ACK_RETRANSMIT) {
            mesh_radio_tx(buf, blen);   /* bus lock taken internally */
        }
    }
    k_work_schedule(&retransmit_work, K_MSEC(CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS));
}

static void route_gc_drop(const uint8_t *dest, void *ctx)
{
    ARG_UNUSED(dest); ARG_UNUSED(ctx);
}

static void route_gc_work_handler(struct k_work *w)
{
    ARG_UNUSED(w);
    if (!mesh_state.started) {
        return;
    }
    uint32_t now = k_uptime_get_32();
    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    mesh_route_gc(&mesh_state.routes, now);
    mesh_pr_gc(&mesh_state.proutes, now, route_gc_drop, NULL);
    k_mutex_unlock(&mesh_state.tables_lock);
    k_work_schedule(&route_gc_work, K_SECONDS(CONFIG_AKIRA_MESH_ROUTE_LIFETIME_S));
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
    mesh_state.node_count = 0;
    mesh_state.seq_num = 0;
    mesh_state.aodv_seq = 0;
    k_mutex_init(&mesh_state.tables_lock);
    mesh_seen_reset(&mesh_state.seen);
    mesh_route_reset(&mesh_state.routes);
    mesh_ack_reset(&mesh_state.acks);
    mesh_pr_reset(&mesh_state.proutes);
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    mesh_session_reset(&mesh_state.sessions);
    if (mesh_crypto_identity_init(mesh_state.identity_priv, mesh_state.identity_pub) != 0) {
        LOG_ERR("AkiraMesh: failed to load/generate node identity keypair");
        k_mutex_unlock(&mesh_init_lock);
        return -EIO;
    }
#endif

    mesh_state.radio = radio_manager_acquire_by_caps(config->transport_caps, "mesh");
    if (!mesh_state.radio) {
        LOG_ERR("No radio matches mesh transport caps 0x%08x", config->transport_caps);
        k_mutex_unlock(&mesh_init_lock);
        return -ENODEV;
    }

    /* BLE transport needs the shared BT stack switched into mesh mode before
     * the radio driver touches it — driven by the acquired radio's generic
     * type, not a hardcoded chip name, so any RADIO_TYPE_BLE driver works. */
#if defined(CONFIG_AKIRA_MESH_TRANSPORT_BLE)
    if (mesh_state.radio->type == RADIO_TYPE_BLE) {
        if (bt_manager_set_mode(BT_MODE_MESH) != 0) {
            LOG_ERR("AkiraMesh: BLE radio busy with another BT mode");
            radio_manager_release(mesh_state.radio, "mesh");
            mesh_state.radio = NULL;
            k_mutex_unlock(&mesh_init_lock);
            return -EBUSY;
        }
    }
#endif

    /* Bring the acquired radio's hardware up — mesh owns it exclusively. */
    if (mesh_state.radio->ops && mesh_state.radio->ops->init) {
        int rret = mesh_state.radio->ops->init(mesh_state.radio);
        if (rret < 0) {
            LOG_ERR("mesh radio '%s' init failed: %d", mesh_state.radio->name, rret);
#if defined(CONFIG_AKIRA_MESH_TRANSPORT_BLE)
            if (mesh_state.radio->type == RADIO_TYPE_BLE) {
                bt_manager_set_mode(BT_MODE_NONE);
            }
#endif
            radio_manager_release(mesh_state.radio, "mesh");
            mesh_state.radio = NULL;
            k_mutex_unlock(&mesh_init_lock);
            return rret;
        }
    }

    /* MTU for this radio's current config. Radios without get_max_payload()
     * keep the historical MESH_PACKET_BUF_SIZE bound (unchanged behavior). */
    mesh_state.mtu = MESH_PACKET_BUF_SIZE;
    if (mesh_state.radio->ops && mesh_state.radio->ops->get_max_payload) {
        size_t max_payload;
        if (mesh_state.radio->ops->get_max_payload(mesh_state.radio, &max_payload) == 0 &&
            max_payload > sizeof(struct mesh_header)) {
            mesh_state.mtu = MIN(max_payload, MESH_PACKET_BUF_SIZE);
        }
    }

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
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    /* Fresh ephemeral prekey each start — rotates naturally on every mesh
     * restart. Continuous-uptime rotation is a separate, unspecified knob,
     * intentionally not added here. */
    if (mesh_crypto_p256_keygen(mesh_state.prekey_priv, mesh_state.prekey_pub) != 0) {
        LOG_ERR("AkiraMesh: failed to generate prekey");
        return -EIO;
    }
#endif
    mesh_state.started = true;
    k_work_schedule(&beacon_work, K_MSEC(beacon_next_delay_ms()));
    k_work_schedule(&retransmit_work, K_MSEC(CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS));
    k_work_schedule(&route_gc_work, K_SECONDS(CONFIG_AKIRA_MESH_ROUTE_LIFETIME_S));
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
    k_work_cancel_delayable(&beacon_work);
    k_work_cancel_delayable(&retransmit_work);
    k_work_cancel_delayable(&route_gc_work);
    if (mesh_state.radio) {
        bool was_ble = (mesh_state.radio->type == RADIO_TYPE_BLE);
        if (mesh_state.radio->ops && mesh_state.radio->ops->deinit) {
            mesh_state.radio->ops->deinit(mesh_state.radio);
        }
        radio_manager_release(mesh_state.radio, "mesh");
        mesh_state.radio = NULL;
#if defined(CONFIG_AKIRA_MESH_TRANSPORT_BLE)
        if (was_ble) {
            bt_manager_set_mode(BT_MODE_NONE);
        }
#endif
    }
    mesh_state.initialized = false;
    k_mutex_unlock(&mesh_init_lock);
    LOG_INF("AkiraMesh stopped");
    return 0;
}

/* Shared reliable-unicast path: build a DATA packet, encrypt it under the
 * cached E2E session for dest_id if one exists, send it if a route exists
 * (tracked for end-to-end ACK/retry), else queue it and originate an RREQ.
 * A missing route and/or session key both resolve off the same RREQ/RREP
 * round trip (see mesh_dispatch's ROUTE_REQ/ROUTE_REPLY handling), so
 * plaintext is never sent as a fallback — hold and retry instead. */
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
static int mesh_send_reliable(const uint8_t *dest_id, const uint8_t *data, size_t len)
{
    uint32_t now = k_uptime_get_32();

    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    struct route_entry *r = mesh_route_lookup(&mesh_state.routes, dest_id, now);
    bool have_route = (r != NULL);
    struct session_entry *sess = mesh_session_lookup(&mesh_state.sessions, dest_id, now);
    uint8_t enc_key[MESH_CRYPTO_SESSION_KEY_LEN];
    uint8_t mac_key[MESH_CRYPTO_MAC_KEY_LEN];
    bool have_key = (sess != NULL);
    if (have_key) {
        memcpy(enc_key, sess->enc_key, sizeof(enc_key));
        memcpy(mac_key, sess->mac_key, sizeof(mac_key));
    }
    k_mutex_unlock(&mesh_state.tables_lock);

    if (!have_route || !have_key) {
        uint8_t pkt[MESH_PACKET_BUF_SIZE];
        struct mesh_header *h = (struct mesh_header *)pkt;
        fill_header(h, AKIRA_MESH_MSG_DATA, mesh_state.config.max_hops, dest_id);
        memcpy(pkt + sizeof(*h), data, len);
        size_t total = sizeof(*h) + len;

        k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
        int pr = mesh_pr_add(&mesh_state.proutes, dest_id, pkt, total,
                             now + CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS *
                                   (CONFIG_AKIRA_MESH_MAX_RETRIES + 1));
        k_mutex_unlock(&mesh_state.tables_lock);
        if (pr < 0) {
            return -EBUSY;
        }
        send_rreq(dest_id);
        return 0;
    }

    uint8_t pkt[MESH_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_header(h, AKIRA_MESH_MSG_DATA, mesh_state.config.max_hops, dest_id);
    uint8_t *body = pkt + sizeof(*h);

    uint8_t nonce[MESH_CRYPTO_NONCE_LEN];
    sys_csrand_get(nonce, sizeof(nonce));
    memcpy(body, nonce, MESH_CRYPTO_NONCE_LEN);
    uint8_t *ct = body + MESH_CRYPTO_NONCE_LEN;
    if (mesh_crypto_aes256_ctr(enc_key, nonce, data, len, ct) != 0) {
        memset(enc_key, 0, sizeof(enc_key));
        memset(mac_key, 0, sizeof(mac_key));
        return -EIO;
    }
    uint8_t tag[32];
    mesh_crypto_hmac_sha256(mac_key, sizeof(mac_key), body,
                            MESH_CRYPTO_NONCE_LEN + len, tag);
    memcpy(ct + len, tag, MESH_CRYPTO_MAC_LEN);
    size_t total = sizeof(*h) + MESH_CRYPTO_NONCE_LEN + len + MESH_CRYPTO_MAC_LEN;
    memset(enc_key, 0, sizeof(enc_key));
    memset(mac_key, 0, sizeof(mac_key));
    memset(tag, 0, sizeof(tag));
    uint16_t seq = h->seq_num;

    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    int slot = mesh_ack_add(&mesh_state.acks, seq, dest_id, pkt, total,
                            now + CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS);
    k_mutex_unlock(&mesh_state.tables_lock);
    if (slot < 0) {
        return -EBUSY;
    }
    mesh_state.stats.messages_sent++;
    return mesh_radio_tx(pkt, total);
}
#endif /* CONFIG_AKIRA_MESH_E2E_CRYPTO */

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
     * broadcast. */
    if (mesh_state.mtu < sizeof(struct mesh_header) + MESH_CRYPTO_OVERHEAD ||
        len > mesh_state.mtu - sizeof(struct mesh_header) - MESH_CRYPTO_OVERHEAD) {
        return -EMSGSIZE;
    }
    return mesh_send_reliable(dest_id, data, len);
#else
    /* 1:1 messages are always encrypted (mandatory, not optional — see
     * mesh_dispatch's DATA case) and this build has no crypto module.
     * Never fall back to plaintext DATA; refuse instead. Broadcast above
     * is unaffected. */
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
    if (len > MESH_PACKET_BUF_SIZE - sizeof(struct mesh_header)) {
        return -EMSGSIZE;
    }
    uint8_t bcast[AKIRA_MESH_NODE_ID_LEN];
    memset(bcast, 0xFF, sizeof(bcast));
    uint8_t pkt[MESH_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_header(h, AKIRA_MESH_MSG_DATA, max_hops ? max_hops : 1, bcast);
    memcpy(pkt + sizeof(*h), data, len);
    mesh_state.stats.messages_sent++;
    return mesh_radio_tx(pkt, sizeof(*h) + len);
}

int akira_mesh_get_nodes(akira_mesh_node_info_t *nodes, size_t max_nodes)
{
    if (!nodes || max_nodes == 0) {
        return -EINVAL;
    }
    if (!mesh_state.initialized) {
        return -ENODEV;
    }
    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    size_t copy_count = MIN(mesh_state.node_count, max_nodes);
    memcpy(nodes, mesh_state.nodes, copy_count * sizeof(akira_mesh_node_info_t));
    k_mutex_unlock(&mesh_state.tables_lock);
    return copy_count;
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

int akira_mesh_distribute_app(const char *app_name, const uint8_t *app_data, size_t app_len)
{
    if (!app_name || !app_data || app_len == 0) {
        return -EINVAL;
    }
    if (!mesh_state.initialized || !mesh_state.started) {
        return -ENODEV;
    }
    /* Chunking, reassembly, and acknowledgments are not yet implemented.
     * Returning 0 previously reported a successful distribution while sending
     * nothing; fail loud with -ENOSYS so callers do not assume delivery. */
    LOG_WRN("Mesh app distribution is unimplemented (app '%s', %zu bytes)",
            app_name, app_len);
    return -ENOSYS;
}
