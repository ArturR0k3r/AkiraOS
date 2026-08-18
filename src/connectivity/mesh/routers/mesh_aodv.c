/**
 * @file mesh_aodv.c
 * @brief AODV routing-protocol implementation, plugged into mesh_router.h.
 *
 * Owns the route table, neighbor table, and beacon/RREQ/RREP/RERR wire
 * logic. Only file that knows AODV's control-frame formats — everything
 * above talks to it exclusively through mesh_router_ops_t.
 *
 * relay_id (on aodv_rreq/aodv_rrep) is rewritten by every hop to its own
 * node_id before re-transmitting — distinct from src_id/target on
 * mesh_header, which always mean "true originator"/"true destination" and
 * never change across hops (DATA/ACK/dedup/E2E-crypto need that). hop_count
 * is incremented by every hop for the same reason: route_entry.hop_count
 * must reflect true hop distance, not a fixed value, since it drives
 * hop-count-based route comparison and diagnostics (forward_data itself
 * only checks route *existence*, never reads next_hop/hop_count — see
 * relay_frame).
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include "mesh_aodv.h"
#include "../mesh_router.h"
#include "../mesh_mac.h"
#include "../mesh_routing.h"
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
#include "../mesh_crypto.h"
#include "ed25519.h"
#endif
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>
#include "lib/mem_helper.h"

LOG_MODULE_REGISTER(akira_mesh_aodv, CONFIG_AKIRA_LOG_LEVEL);

#define MESH_ROUTE_LIFETIME_MS (CONFIG_AKIRA_MESH_ROUTE_LIFETIME_S * 1000U)

struct __packed aodv_rreq {
    uint8_t  target[AKIRA_MESH_NODE_ID_LEN];
    uint16_t rreq_id;
    uint16_t orig_seq;
    uint16_t dest_seq;
    uint8_t  hop_count;
    uint8_t  relay_id[AKIRA_MESH_NODE_ID_LEN];
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    uint8_t  orig_identity_pub[MESH_CRYPTO_PUB_LEN];
#endif
};
struct __packed aodv_rrep {
    uint8_t  target[AKIRA_MESH_NODE_ID_LEN];
    uint16_t dest_seq;
    uint8_t  hop_count;
    uint8_t  relay_id[AKIRA_MESH_NODE_ID_LEN];
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    uint8_t  target_prekey_pub[MESH_CRYPTO_PUB_LEN];
#endif
};
struct __packed aodv_rerr {
    uint8_t  unreachable[AKIRA_MESH_NODE_ID_LEN];
    uint16_t dest_seq;
};

#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
/* Sent as a separate follow-up frame instead of inline in aodv_rreq/rrep —
 * Ed25519 sig+pubkey is a fixed 96B that CC1121's 125B FIFO has no room
 * for on top of the base frame (already 109B/105B). Correlated to its
 * base frame by (h->src_id, seq) — see sign_pending/pending_get(). */
struct __packed aodv_rreq_sig {
    uint16_t orig_seq;
    uint8_t  sign_pub[MESH_CRYPTO_SIGN_PUB_LEN];
    uint8_t  sig[MESH_CRYPTO_SIGN_LEN];
};
struct __packed aodv_rrep_sig {
    uint16_t dest_seq;
    uint8_t  sign_pub[MESH_CRYPTO_SIGN_PUB_LEN];
    uint8_t  sig[MESH_CRYPTO_SIGN_LEN];
};
#endif

static struct {
    akira_mesh_config_t     config;
    akira_mesh_stats_t     *stats;
    uint16_t                seq_num;
    uint16_t                aodv_seq;
    akira_mesh_node_info_t  nodes[AKIRA_MESH_MAX_NODES];
    uint8_t                 node_count;
    struct route_table      routes;
    struct pending_route_q  proutes;
    struct seen_cache       seen;
    struct k_mutex          lock;
    bool                    started;
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    uint8_t                 identity_priv[MESH_CRYPTO_PRIV_LEN];
    uint8_t                 identity_pub[MESH_CRYPTO_PUB_LEN];
    uint8_t                 prekey_priv[MESH_CRYPTO_PRIV_LEN];
    uint8_t                 prekey_pub[MESH_CRYPTO_PUB_LEN];
#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
    uint8_t                 sign_priv[MESH_CRYPTO_SIGN_PRIV_LEN];
    uint8_t                 sign_pub[MESH_CRYPTO_SIGN_PUB_LEN];
#endif
#endif
} s_aodv AKIRA_BULK_BSS;

#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
/* TOFU pin store for RREQ/RREP signing keys, keyed by the signer's node ID
 * (RREQ: h->src_id/originator; RREP: rp->target/responder, both always
 * equal to h->src_id on that frame type — see pending_get() callers).
 * First sig-verified message from a node ID pins its key; a later message
 * from the same ID with a different key is rejected. Doesn't stop a MITM
 * present from the very first contact (inherent TOFU limitation) — it
 * stops a relay swapping keys afterward. */
struct sign_pin {
    uint8_t node_id[AKIRA_MESH_NODE_ID_LEN];
    uint8_t sign_pub[MESH_CRYPTO_SIGN_PUB_LEN];
    bool    valid;
};
static struct sign_pin s_sign_pins[CONFIG_AKIRA_MESH_MAX_PINS];
static size_t s_sign_pin_evict_next;

static bool pin_check_and_learn(const uint8_t *node_id, const uint8_t *sign_pub)
{
    for (size_t i = 0; i < CONFIG_AKIRA_MESH_MAX_PINS; i++) {
        if (s_sign_pins[i].valid &&
            memcmp(s_sign_pins[i].node_id, node_id, AKIRA_MESH_NODE_ID_LEN) == 0) {
            return memcmp(s_sign_pins[i].sign_pub, sign_pub, MESH_CRYPTO_SIGN_PUB_LEN) == 0;
        }
    }
    struct sign_pin *slot = NULL;
    for (size_t i = 0; i < CONFIG_AKIRA_MESH_MAX_PINS; i++) {
        if (!s_sign_pins[i].valid) {
            slot = &s_sign_pins[i];
            break;
        }
    }
    if (!slot) {
        slot = &s_sign_pins[s_sign_pin_evict_next];
        s_sign_pin_evict_next = (s_sign_pin_evict_next + 1) % CONFIG_AKIRA_MESH_MAX_PINS;
    }
    memcpy(slot->node_id, node_id, AKIRA_MESH_NODE_ID_LEN);
    memcpy(slot->sign_pub, sign_pub, MESH_CRYPTO_SIGN_PUB_LEN);
    slot->valid = true;
    return true;
}

/* A RREQ/RREP and its _SIG follow-up are sent back-to-back but arrive
 * independently over the radio — CSMA requeue-on-busy can reorder frames
 * within the same lane, so either piece can land first. One slot holds
 * whichever piece arrived, keyed by (node_id, seq); the other handler
 * (handle_route_req/_sig, handle_route_reply/_sig) completes the pair. */
struct sign_pending {
    bool     valid, have_base, have_sig;
    uint8_t  node_id[AKIRA_MESH_NODE_ID_LEN];
    uint16_t seq;
    uint32_t expires_ms;
    uint8_t  base_buf[MESH_MAC_PACKET_BUF_SIZE];
    size_t   base_len;
    uint8_t  sig_buf[sizeof(struct mesh_header) + sizeof(struct aodv_rreq_sig)];
    size_t   sig_len;
};
static struct sign_pending s_pending_rreq[CONFIG_AKIRA_MESH_SIGN_PENDING_MAX];
static struct sign_pending s_pending_rrep[CONFIG_AKIRA_MESH_SIGN_PENDING_MAX];

/* Finds the (node_id, seq) slot, or allocates one (free slot, else evicts
 * the soonest-expiring — mirrors mesh_session_install's eviction). */
static struct sign_pending *pending_get(struct sign_pending *table, const uint8_t *node_id,
                                        uint16_t seq, uint32_t now)
{
    for (size_t i = 0; i < CONFIG_AKIRA_MESH_SIGN_PENDING_MAX; i++) {
        if (table[i].valid && table[i].seq == seq &&
            memcmp(table[i].node_id, node_id, AKIRA_MESH_NODE_ID_LEN) == 0) {
            return &table[i];
        }
    }
    struct sign_pending *slot = NULL;
    for (size_t i = 0; i < CONFIG_AKIRA_MESH_SIGN_PENDING_MAX; i++) {
        if (!table[i].valid) {
            slot = &table[i];
            break;
        }
    }
    if (!slot) {
        slot = &table[0];
        for (size_t i = 1; i < CONFIG_AKIRA_MESH_SIGN_PENDING_MAX; i++) {
            if ((int32_t)(table[i].expires_ms - slot->expires_ms) < 0) {
                slot = &table[i];
            }
        }
    }
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->node_id, node_id, AKIRA_MESH_NODE_ID_LEN);
    slot->seq = seq;
    slot->expires_ms = now + CONFIG_AKIRA_MESH_SIGN_WAIT_MS;
    slot->valid = true;
    return slot;
}

/* Caller holds s_aodv.lock — s_pending_rreq/s_pending_rrep are also written
 * from the RX-dispatch handlers under that same lock. */
static void sign_pending_gc(uint32_t now)
{
    for (size_t i = 0; i < CONFIG_AKIRA_MESH_SIGN_PENDING_MAX; i++) {
        if (s_pending_rreq[i].valid && (int32_t)(now - s_pending_rreq[i].expires_ms) >= 0) {
            memset(&s_pending_rreq[i], 0, sizeof(s_pending_rreq[i]));
        }
        if (s_pending_rrep[i].valid && (int32_t)(now - s_pending_rrep[i].expires_ms) >= 0) {
            memset(&s_pending_rrep[i], 0, sizeof(s_pending_rrep[i]));
        }
    }
}

#define RREQ_SIGN_MSG_LEN (MESH_CRYPTO_PUB_LEN + 2 * AKIRA_MESH_NODE_ID_LEN + sizeof(uint16_t))

static size_t build_rreq_sign_msg(uint8_t *out, const uint8_t *orig_identity_pub,
                                  const uint8_t *src_id, const uint8_t *target, uint16_t orig_seq)
{
    size_t off = 0;
    memcpy(out + off, orig_identity_pub, MESH_CRYPTO_PUB_LEN); off += MESH_CRYPTO_PUB_LEN;
    memcpy(out + off, src_id, AKIRA_MESH_NODE_ID_LEN); off += AKIRA_MESH_NODE_ID_LEN;
    memcpy(out + off, target, AKIRA_MESH_NODE_ID_LEN); off += AKIRA_MESH_NODE_ID_LEN;
    memcpy(out + off, &orig_seq, sizeof(orig_seq)); off += sizeof(orig_seq);
    return off;
}

static void sign_rreq(const struct aodv_rreq *rq, uint8_t sig_out[MESH_CRYPTO_SIGN_LEN])
{
    uint8_t msg[RREQ_SIGN_MSG_LEN];
    size_t len = build_rreq_sign_msg(msg, rq->orig_identity_pub, s_aodv.config.node_id,
                                     rq->target, rq->orig_seq);
    ed25519_sign(s_aodv.sign_priv, msg, len, sig_out);
}

static bool check_rreq_sig(const struct aodv_rreq *rq, const uint8_t *src_id,
                           const uint8_t *sign_pub, const uint8_t *sig)
{
    uint8_t msg[RREQ_SIGN_MSG_LEN];
    size_t len = build_rreq_sign_msg(msg, rq->orig_identity_pub, src_id, rq->target, rq->orig_seq);
    if (ed25519_verify(sign_pub, msg, len, sig) != 0) {
        return false;
    }
    return pin_check_and_learn(src_id, sign_pub);
}

#define RREP_SIGN_MSG_LEN (MESH_CRYPTO_PUB_LEN + AKIRA_MESH_NODE_ID_LEN + sizeof(uint16_t))

static size_t build_rrep_sign_msg(uint8_t *out, const uint8_t *target_prekey_pub,
                                  const uint8_t *target, uint16_t dest_seq)
{
    size_t off = 0;
    memcpy(out + off, target_prekey_pub, MESH_CRYPTO_PUB_LEN); off += MESH_CRYPTO_PUB_LEN;
    memcpy(out + off, target, AKIRA_MESH_NODE_ID_LEN); off += AKIRA_MESH_NODE_ID_LEN;
    memcpy(out + off, &dest_seq, sizeof(dest_seq)); off += sizeof(dest_seq);
    return off;
}

static void sign_rrep(const struct aodv_rrep *rp, uint8_t sig_out[MESH_CRYPTO_SIGN_LEN])
{
    uint8_t msg[RREP_SIGN_MSG_LEN];
    size_t len = build_rrep_sign_msg(msg, rp->target_prekey_pub, rp->target, rp->dest_seq);
    ed25519_sign(s_aodv.sign_priv, msg, len, sig_out);
}

static bool check_rrep_sig(const struct aodv_rrep *rp, const uint8_t *sign_pub, const uint8_t *sig)
{
    uint8_t msg[RREP_SIGN_MSG_LEN];
    size_t len = build_rrep_sign_msg(msg, rp->target_prekey_pub, rp->target, rp->dest_seq);
    if (ed25519_verify(sign_pub, msg, len, sig) != 0) {
        return false;
    }
    return pin_check_and_learn(rp->target, sign_pub);
}
#endif /* CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING */

static bool is_self(const uint8_t *id)
{
    return memcmp(id, s_aodv.config.node_id, AKIRA_MESH_NODE_ID_LEN) == 0;
}

static void fill_aodv_header(struct mesh_header *h, uint8_t type, uint8_t ttl, const uint8_t *dest)
{
    h->version = 1;
    h->msg_type = type;
    h->ttl = ttl;
    memcpy(h->src_id, s_aodv.config.node_id, AKIRA_MESH_NODE_ID_LEN);
    memcpy(h->dest_id, dest, AKIRA_MESH_NODE_ID_LEN);
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    h->seq_num = s_aodv.seq_num++;
    k_mutex_unlock(&s_aodv.lock);
}

/* send_rreq runs from multiple threads (shell, RX-dispatch relay/local-repair
 * paths, sysworkq via aodv_tick's mesh_pr_tick) — the increment must be
 * locked like seq_num above, not a bare read-modify-write. Also used when
 * replying (handle_route_req_verified) so a pure-responder node's aodv_seq
 * advances too: without that, every RREP it ever sends carries dest_seq=0,
 * which under CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING signs byte-identical
 * messages every time (see build_rrep_sign_msg). */
static uint16_t next_aodv_seq(void)
{
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    uint16_t v = ++s_aodv.aodv_seq;
    k_mutex_unlock(&s_aodv.lock);
    return v;
}

int mesh_aodv_module_init(const akira_mesh_config_t *config, akira_mesh_stats_t *stats)
{
    memcpy(&s_aodv.config, config, sizeof(*config));
    s_aodv.stats = stats;
    k_mutex_init(&s_aodv.lock);
    mesh_route_reset(&s_aodv.routes);
    mesh_pr_reset(&s_aodv.proutes);
    mesh_seen_reset(&s_aodv.seen);
    s_aodv.node_count = 0;
    s_aodv.seq_num = 0;
    s_aodv.aodv_seq = 0;
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    if (mesh_crypto_identity_init(s_aodv.identity_priv, s_aodv.identity_pub) != 0) {
        LOG_ERR("AkiraMesh AODV: failed to load/generate node identity keypair");
        return -EIO;
    }
#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
    if (mesh_crypto_signing_identity_init(s_aodv.sign_priv, s_aodv.sign_pub) != 0) {
        LOG_ERR("AkiraMesh AODV: failed to load/generate node signing keypair");
        return -EIO;
    }
#endif
#endif
    return 0;
}

int mesh_aodv_get_nodes(akira_mesh_node_info_t *nodes, size_t max_nodes)
{
    if (!nodes || max_nodes == 0) {
        return -EINVAL;
    }
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    size_t copy_count = MIN(s_aodv.node_count, max_nodes);
    memcpy(nodes, s_aodv.nodes, copy_count * sizeof(akira_mesh_node_info_t));
    k_mutex_unlock(&s_aodv.lock);
    return (int)copy_count;
}

/* ------------------------------------------------------------------ */
/* Neighbor table + beacon                                             */
/* ------------------------------------------------------------------ */

void mesh_aodv_dispatch_beacon(const uint8_t *src_id, const uint8_t *payload, size_t plen,
                               int16_t rssi)
{
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    for (uint8_t i = 0; i < s_aodv.node_count; i++) {
        if (memcmp(s_aodv.nodes[i].node_id, src_id, AKIRA_MESH_NODE_ID_LEN) == 0) {
            s_aodv.nodes[i].last_seen = k_uptime_get_32();
            s_aodv.nodes[i].rssi = (int8_t)CLAMP(rssi, INT8_MIN, INT8_MAX);
            if (s_aodv.nodes[i].name[0] == '\0' && plen > 0 &&
                plen < sizeof(s_aodv.nodes[i].name)) {
                memcpy(s_aodv.nodes[i].name, payload, plen);
                s_aodv.nodes[i].name[plen] = '\0';
                LOG_INF("Updated name for known node: %s", s_aodv.nodes[i].name);
            }
            k_mutex_unlock(&s_aodv.lock);
            return;
        }
    }
    if (s_aodv.node_count < AKIRA_MESH_MAX_NODES) {
        akira_mesh_node_info_t *node = &s_aodv.nodes[s_aodv.node_count++];
        memset(node, 0, sizeof(*node));
        memcpy(node->node_id, src_id, AKIRA_MESH_NODE_ID_LEN);
        if (plen > 0 && plen < sizeof(node->name)) {
            memcpy(node->name, payload, plen);
            node->name[plen] = '\0';
        }
        node->hop_count = 1;
        node->rssi = (int8_t)CLAMP(rssi, INT8_MIN, INT8_MAX);
        node->last_seen = k_uptime_get_32();
        node->role = AKIRA_MESH_ROLE_NODE;
        if (s_aodv.stats) {
            s_aodv.stats->nodes_discovered++;
        }
        LOG_INF("Discovered mesh node: %s", node->name);
    }
    k_mutex_unlock(&s_aodv.lock);
}

/* Drop neighbors not heard from in a while so a mobile mesh keeps
 * discovering new nodes instead of sticking at AKIRA_MESH_MAX_NODES forever
 * with stale entries. */
static void neighbor_gc(uint32_t now)
{
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    for (int i = 0; i < s_aodv.node_count; ) {
        if (now - s_aodv.nodes[i].last_seen > CONFIG_AKIRA_MESH_NEIGHBOR_TIMEOUT_MS) {
            s_aodv.nodes[i] = s_aodv.nodes[s_aodv.node_count - 1];
            s_aodv.node_count--;
        } else {
            i++;
        }
    }
    k_mutex_unlock(&s_aodv.lock);
}

/* +-20% jitter so two nodes started together don't stay beacon-phase-locked:
 * with a fixed period, nodes whose beacon windows align on a half-duplex
 * radio never break the tie and never hear each other. */
static uint32_t beacon_next_delay_ms(void)
{
    int32_t range = (int32_t)s_aodv.config.beacon_interval_ms / 5;
    int32_t jitter = (int32_t)(sys_rand32_get() % (2 * range + 1)) - range;
    return s_aodv.config.beacon_interval_ms + jitter;
}

static void beacon_work_handler(struct k_work *w);
K_WORK_DELAYABLE_DEFINE(beacon_work, beacon_work_handler);

static void beacon_work_handler(struct k_work *w)
{
    ARG_UNUSED(w);
    if (!s_aodv.started) {
        return;
    }
    uint8_t bcast[AKIRA_MESH_NODE_ID_LEN];
    memset(bcast, 0xFF, sizeof(bcast));
    uint8_t pkt[64];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_aodv_header(h, AKIRA_MESH_MSG_BEACON, 1, bcast);
    size_t nlen = strlen(s_aodv.config.node_name);
    if (sizeof(*h) + nlen > sizeof(pkt)) {
        nlen = sizeof(pkt) - sizeof(*h);
    }
    memcpy(pkt + sizeof(*h), s_aodv.config.node_name, nlen);
    mesh_mac_send(MESH_MAC_PRIO_CRITICAL, pkt, sizeof(*h) + nlen);
    if (s_aodv.stats) {
        s_aodv.stats->messages_sent++;
    }
    k_work_schedule(&beacon_work, K_MSEC(beacon_next_delay_ms()));
}

/* ------------------------------------------------------------------ */
/* AODV control-frame TX builders                                      */
/* ------------------------------------------------------------------ */

/* The seen-cache is otherwise only populated on receive — a node has no way
 * to recognize its own broadcast echoing back via a relay (dedup misses,
 * gets reprocessed and re-relayed again). Marking (self_id, seq_num) seen
 * the moment we send it closes that gap for RREQ/RREP/RERR alike. */
static void mark_own_frame_seen(uint16_t seq_num)
{
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    mesh_seen_check_and_add(&s_aodv.seen, s_aodv.config.node_id, seq_num);
    k_mutex_unlock(&s_aodv.lock);
}

#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
static void send_rreq_sig(const struct aodv_rreq *rq)
{
    uint8_t bcast[AKIRA_MESH_NODE_ID_LEN];
    memset(bcast, 0xFF, sizeof(bcast));
    uint8_t pkt[sizeof(struct mesh_header) + sizeof(struct aodv_rreq_sig)];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_aodv_header(h, AKIRA_MESH_MSG_ROUTE_REQ_SIG, s_aodv.config.max_hops, bcast);
    mark_own_frame_seen(h->seq_num);
    struct aodv_rreq_sig *rs = (struct aodv_rreq_sig *)(pkt + sizeof(*h));
    rs->orig_seq = rq->orig_seq;
    memcpy(rs->sign_pub, s_aodv.sign_pub, MESH_CRYPTO_SIGN_PUB_LEN);
    sign_rreq(rq, rs->sig);
    mesh_mac_send(MESH_MAC_PRIO_CRITICAL, pkt, sizeof(pkt));
}

static void send_rrep_sig(const uint8_t *to_immediate, const struct aodv_rrep *rp)
{
    uint8_t pkt[sizeof(struct mesh_header) + sizeof(struct aodv_rrep_sig)];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_aodv_header(h, AKIRA_MESH_MSG_ROUTE_REPLY_SIG, s_aodv.config.max_hops, to_immediate);
    mark_own_frame_seen(h->seq_num);
    struct aodv_rrep_sig *rs = (struct aodv_rrep_sig *)(pkt + sizeof(*h));
    rs->dest_seq = rp->dest_seq;
    memcpy(rs->sign_pub, s_aodv.sign_pub, MESH_CRYPTO_SIGN_PUB_LEN);
    sign_rrep(rp, rs->sig);
    mesh_mac_send(MESH_MAC_PRIO_CRITICAL, pkt, sizeof(pkt));
}
#endif

/* void *ctx parameter unused directly — signature matches mesh_pr_tick's
 * on_retry callback type exactly so it can be passed without a cast (a
 * mismatched-signature function pointer cast is UB in C and breaks under
 * CFI/-fsanitize=function even when it happens to work in practice). */
static void send_rreq(const uint8_t *target, void *ctx)
{
    ARG_UNUSED(ctx);
    uint8_t bcast[AKIRA_MESH_NODE_ID_LEN];
    memset(bcast, 0xFF, sizeof(bcast));
    uint8_t pkt[sizeof(struct mesh_header) + sizeof(struct aodv_rreq)];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_aodv_header(h, AKIRA_MESH_MSG_ROUTE_REQ, s_aodv.config.max_hops, bcast);
    mark_own_frame_seen(h->seq_num);
    struct aodv_rreq *rq = (struct aodv_rreq *)(pkt + sizeof(*h));
    memcpy(rq->target, target, AKIRA_MESH_NODE_ID_LEN);
    rq->rreq_id = h->seq_num;
    rq->orig_seq = next_aodv_seq();
    rq->dest_seq = 0;
    rq->hop_count = 0;
    memcpy(rq->relay_id, s_aodv.config.node_id, AKIRA_MESH_NODE_ID_LEN);
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    memcpy(rq->orig_identity_pub, s_aodv.identity_pub, MESH_CRYPTO_PUB_LEN);
#endif
    LOG_INF("AODV: RREQ tx target=%02x seq=%u", target[AKIRA_MESH_NODE_ID_LEN - 1], rq->orig_seq);
    mesh_mac_send(MESH_MAC_PRIO_CRITICAL, pkt, sizeof(pkt));
#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
    send_rreq_sig(rq);
#endif
}

static void send_rrep(const uint8_t *to_immediate, const uint8_t *target,
                      uint16_t dest_seq, uint8_t hop_count)
{
    uint8_t pkt[sizeof(struct mesh_header) + sizeof(struct aodv_rrep)];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_aodv_header(h, AKIRA_MESH_MSG_ROUTE_REPLY, s_aodv.config.max_hops, to_immediate);
    mark_own_frame_seen(h->seq_num);
    struct aodv_rrep *rp = (struct aodv_rrep *)(pkt + sizeof(*h));
    memcpy(rp->target, target, AKIRA_MESH_NODE_ID_LEN);
    rp->dest_seq = dest_seq;
    rp->hop_count = hop_count;
    memcpy(rp->relay_id, s_aodv.config.node_id, AKIRA_MESH_NODE_ID_LEN);
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    memcpy(rp->target_prekey_pub, s_aodv.prekey_pub, MESH_CRYPTO_PUB_LEN);
#endif
    LOG_INF("AODV: RREP tx to=%02x target=%02x hop=%u", to_immediate[AKIRA_MESH_NODE_ID_LEN - 1],
            target[AKIRA_MESH_NODE_ID_LEN - 1], hop_count);
    mesh_mac_send(MESH_MAC_PRIO_CRITICAL, pkt, sizeof(pkt));
#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
    send_rrep_sig(to_immediate, rp);
#endif
}

/* Broadcast, not unicast to the immediate sender: every one-hop neighbor
 * using this node as next-hop for the dead route overhears it and
 * invalidates; everyone else no-ops. Gets precursor-style propagation for
 * free with no per-route precursor-list bookkeeping, because on this
 * broadcast medium a route's precursors are, by definition, this node's
 * one-hop neighbors already. */
static void send_rerr_broadcast(const uint8_t *unreachable)
{
    uint8_t bcast[AKIRA_MESH_NODE_ID_LEN];
    memset(bcast, 0xFF, sizeof(bcast));
    uint8_t pkt[sizeof(struct mesh_header) + sizeof(struct aodv_rerr)];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_aodv_header(h, AKIRA_MESH_MSG_ROUTE_ERROR, 1, bcast);
    mark_own_frame_seen(h->seq_num);
    struct aodv_rerr *re = (struct aodv_rerr *)(pkt + sizeof(*h));
    memcpy(re->unreachable, unreachable, AKIRA_MESH_NODE_ID_LEN);
    re->dest_seq = 0;
    LOG_INF("AODV: RERR tx unreachable=%02x", unreachable[AKIRA_MESH_NODE_ID_LEN - 1]);
    mesh_mac_send(MESH_MAC_PRIO_CRITICAL, pkt, sizeof(pkt));
}

/* ------------------------------------------------------------------ */
/* Forwarding + local repair + pending-route flush                     */
/* ------------------------------------------------------------------ */

static void relay_frame(struct mesh_header *h, const uint8_t *full, size_t len)
{
    if (h->ttl == 0) {
        return;
    }
    uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
    if (len > sizeof(pkt)) {
        return;
    }
    memcpy(pkt, full, len);
    ((struct mesh_header *)pkt)->ttl = h->ttl - 1;
    if (s_aodv.stats) {
        s_aodv.stats->messages_forwarded++;
    }
    mesh_mac_send(mesh_mac_prio_for_msg_type(h->msg_type), pkt, len);
}

/* Called from mesh_pr_gc's on_drop when an entry expires. Only a
 * local-repair entry (a route through this node existed and broke) means
 * anything to announce — a source-side send that never found any route in
 * the first place has nothing to invalidate for its neighbors. */
static void pending_route_drop(const uint8_t *dest, bool is_local_repair, void *ctx)
{
    ARG_UNUSED(ctx);
    if (is_local_repair) {
        send_rerr_broadcast(dest);
    }
}

static void forward_data(struct mesh_header *h, const uint8_t *full, size_t len)
{
    if (h->ttl == 0) {
        return;
    }
    uint32_t now = k_uptime_get_32();
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    struct route_entry *r = mesh_route_lookup(&s_aodv.routes, h->dest_id, now);
    bool have = (r != NULL);
    if (have) {
        /* Active relay traffic — push expiry out so a route in continuous
         * use (e.g. mid app-chunk transfer) doesn't hard-expire on its
         * fixed install-time lifetime and force a mid-transfer RREQ. */
        mesh_route_touch(&s_aodv.routes, h->dest_id, now, now + MESH_ROUTE_LIFETIME_MS);
    }
    k_mutex_unlock(&s_aodv.lock);
    if (have) {
        relay_frame(h, full, len);
        return;
    }
    if (h->ttl <= 1) {
        /* No budget left to even attempt a local repair round trip. */
        send_rerr_broadcast(h->dest_id);
        return;
    }
    /* Local repair: queue and try our own RREQ before bouncing an error back
     * to the original source — reuses the exact same "no route -> queue +
     * RREQ" machinery the source side uses (aodv_queue_pending below), just
     * one hop earlier. */
    LOG_INF("AODV: local repair for dest=%02x (no route, ttl=%u)",
            h->dest_id[AKIRA_MESH_NODE_ID_LEN - 1], h->ttl);
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    int pr = mesh_pr_add(&s_aodv.proutes, h->dest_id, full, (uint16_t)len, now,
                         now + CONFIG_AKIRA_MESH_LOCAL_REPAIR_TIMEOUT_MS, true);
    k_mutex_unlock(&s_aodv.lock);
    if (pr < 0) {
        send_rerr_broadcast(h->dest_id);
        return;
    }
    send_rreq(h->dest_id, NULL);
}

static void flush_pending_for(const uint8_t *dest)
{
    while (1) {
        k_mutex_lock(&s_aodv.lock, K_FOREVER);
        struct pending_route *e = mesh_pr_next_for_dest(&s_aodv.proutes, dest);
        if (!e) { k_mutex_unlock(&s_aodv.lock); break; }
        uint8_t buf[MESH_MAC_PACKET_BUF_SIZE];
        uint16_t l = e->len;
        if (l > sizeof(buf)) {
            l = sizeof(buf);
        }
        memcpy(buf, e->payload, l);
        mesh_pr_clear_slot(e);
        k_mutex_unlock(&s_aodv.lock);
        if (l < sizeof(struct mesh_header)) {
            continue;
        }
        /* Full stored packet (header+body), queued either by a source-side
         * send (Transport, via queue_pending) or by this file's own local
         * repair — re-transmit it as-is, TTL already correct from when it
         * was queued. */
        struct mesh_header *bh = (struct mesh_header *)buf;
        mesh_mac_send(mesh_mac_prio_for_msg_type(bh->msg_type), buf, l);
    }
}

/* ------------------------------------------------------------------ */
/* RREQ/RREP/RERR handling                                             */
/* ------------------------------------------------------------------ */

static void handle_route_req_verified(struct mesh_header *h, const uint8_t *buf, size_t len,
                                      const struct aodv_rreq *rq)
{
    uint32_t now = k_uptime_get_32();
    LOG_INF("AODV: RREQ rx orig=%02x via=%02x hop=%u target=%02x",
            h->src_id[AKIRA_MESH_NODE_ID_LEN - 1], rq->relay_id[AKIRA_MESH_NODE_ID_LEN - 1],
            rq->hop_count, rq->target[AKIRA_MESH_NODE_ID_LEN - 1]);
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    mesh_seen_add(&s_aodv.seen, h->src_id, h->seq_num);
    mesh_route_install(&s_aodv.routes, h->src_id, rq->relay_id,
                       rq->hop_count + 1, rq->orig_seq, now, now + MESH_ROUTE_LIFETIME_MS);
    k_mutex_unlock(&s_aodv.lock);

    if (is_self(rq->target)) {
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
        if (mesh_router_derive_and_install_session(s_aodv.prekey_priv, s_aodv.config.node_id,
                                                    rq->orig_identity_pub, h->src_id, now,
                                                    CONFIG_AKIRA_MESH_SESSION_LIFETIME_S * 1000U) != 0) {
            LOG_WRN("AkiraMesh: ECDH failed deriving session for RREQ from %02x%02x",
                    h->src_id[0], h->src_id[1]);
        }
#endif
        send_rrep(h->src_id, s_aodv.config.node_id, next_aodv_seq(), 0);
    } else if (h->ttl > 1) {
        uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
        if (len <= sizeof(pkt)) {
            memcpy(pkt, buf, len);
            struct mesh_header *ph = (struct mesh_header *)pkt;
            struct aodv_rreq *prq = (struct aodv_rreq *)(pkt + sizeof(*ph));
            ph->ttl = h->ttl - 1;
            prq->hop_count = rq->hop_count + 1;
            memcpy(prq->relay_id, s_aodv.config.node_id, AKIRA_MESH_NODE_ID_LEN);
            LOG_INF("AODV: RREQ relay orig=%02x target=%02x new_hop=%u ttl=%u",
                    h->src_id[AKIRA_MESH_NODE_ID_LEN - 1], rq->target[AKIRA_MESH_NODE_ID_LEN - 1],
                    prq->hop_count, ph->ttl);
            mesh_mac_send(MESH_MAC_PRIO_CRITICAL, pkt, len);
        }
    }
}

static void handle_route_req(struct mesh_header *h, const uint8_t *buf, size_t len,
                             const uint8_t *payload, size_t plen)
{
    if (plen < sizeof(struct aodv_rreq)) {
        return;
    }
    if (is_self(h->src_id)) {
        return; /* our own RREQ, rebroadcast back to us */
    }
    const struct aodv_rreq *rq = (const struct aodv_rreq *)payload;
#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
    uint32_t now = k_uptime_get_32();
    uint8_t sig_buf[sizeof(struct mesh_header) + sizeof(struct aodv_rreq_sig)];
    size_t sig_len = 0;
    bool have_sig;
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    struct sign_pending *p = pending_get(s_pending_rreq, h->src_id, rq->orig_seq, now);
    have_sig = p->have_sig;
    if (!have_sig) {
        p->have_base = true;
        memcpy(p->base_buf, buf, MIN(len, sizeof(p->base_buf)));
        p->base_len = len;
    } else {
        sig_len = p->sig_len;
        memcpy(sig_buf, p->sig_buf, sig_len);
        memset(p, 0, sizeof(*p));
    }
    k_mutex_unlock(&s_aodv.lock);
    if (!have_sig) {
        return;
    }

    const struct aodv_rreq_sig *rs =
        (const struct aodv_rreq_sig *)(sig_buf + sizeof(struct mesh_header));
    if (!check_rreq_sig(rq, h->src_id, rs->sign_pub, rs->sig)) {
        LOG_WRN("AODV: RREQ sig check failed orig=%02x", h->src_id[AKIRA_MESH_NODE_ID_LEN - 1]);
        return;
    }
    struct mesh_header *sh = (struct mesh_header *)sig_buf;
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    mesh_seen_add(&s_aodv.seen, sh->src_id, sh->seq_num);
    k_mutex_unlock(&s_aodv.lock);
    relay_frame(sh, sig_buf, sig_len);
#endif
    handle_route_req_verified(h, buf, len, rq);
}

#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
static void handle_route_req_sig(struct mesh_header *h, const uint8_t *buf, size_t len,
                                 const uint8_t *payload, size_t plen)
{
    if (plen < sizeof(struct aodv_rreq_sig)) {
        return;
    }
    if (is_self(h->src_id)) {
        return;
    }
    const struct aodv_rreq_sig *rs = (const struct aodv_rreq_sig *)payload;
    uint32_t now = k_uptime_get_32();
    uint8_t base_buf[MESH_MAC_PACKET_BUF_SIZE];
    size_t base_len = 0;
    bool have_base;
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    struct sign_pending *p = pending_get(s_pending_rreq, h->src_id, rs->orig_seq, now);
    have_base = p->have_base;
    if (!have_base) {
        p->have_sig = true;
        memcpy(p->sig_buf, buf, MIN(len, sizeof(p->sig_buf)));
        p->sig_len = len;
    } else {
        base_len = p->base_len;
        memcpy(base_buf, p->base_buf, base_len);
        memset(p, 0, sizeof(*p));
    }
    k_mutex_unlock(&s_aodv.lock);
    if (!have_base) {
        return;
    }

    struct mesh_header *bh = (struct mesh_header *)base_buf;
    const struct aodv_rreq *rq = (const struct aodv_rreq *)(base_buf + sizeof(struct mesh_header));
    if (!check_rreq_sig(rq, bh->src_id, rs->sign_pub, rs->sig)) {
        LOG_WRN("AODV: RREQ sig check failed orig=%02x", bh->src_id[AKIRA_MESH_NODE_ID_LEN - 1]);
        return;
    }
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    mesh_seen_add(&s_aodv.seen, h->src_id, h->seq_num);
    k_mutex_unlock(&s_aodv.lock);
    relay_frame(h, buf, len);
    handle_route_req_verified(bh, base_buf, base_len, rq);
}
#endif

static void handle_route_reply_verified(struct mesh_header *h, const uint8_t *buf, size_t len,
                                        const struct aodv_rrep *rp)
{
    uint32_t now = k_uptime_get_32();
    LOG_INF("AODV: RREP rx replier=%02x heading_to=%02x via=%02x hop=%u",
            rp->target[AKIRA_MESH_NODE_ID_LEN - 1], h->dest_id[AKIRA_MESH_NODE_ID_LEN - 1],
            rp->relay_id[AKIRA_MESH_NODE_ID_LEN - 1], rp->hop_count);
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    mesh_seen_add(&s_aodv.seen, h->src_id, h->seq_num);
    mesh_route_install(&s_aodv.routes, rp->target, rp->relay_id,
                       rp->hop_count + 1, rp->dest_seq, now, now + MESH_ROUTE_LIFETIME_MS);
    k_mutex_unlock(&s_aodv.lock);

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    if (is_self(h->dest_id)) {
        if (mesh_router_derive_and_install_session(s_aodv.identity_priv, s_aodv.config.node_id,
                                                    rp->target_prekey_pub, rp->target, now,
                                                    CONFIG_AKIRA_MESH_SESSION_LIFETIME_S * 1000U) != 0) {
            LOG_WRN("AkiraMesh: ECDH failed deriving session for RREP from %02x%02x",
                    rp->target[0], rp->target[1]);
        }
    }
#endif
    flush_pending_for(rp->target);
    if (!is_self(h->dest_id) && h->ttl > 1) {
        /* Bump hop_count/relay_id before handing off to forward_data, same
         * as the RREQ relay branch — forward_data itself only touches ttl,
         * it doesn't know this is a RREP payload. Route existence toward
         * h->dest_id (the RREQ originator) was populated earlier when this
         * node first relayed that RREQ — if it's since expired, forward_data
         * runs the same local-repair path any other relay miss would. */
        uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
        if (len <= sizeof(pkt)) {
            memcpy(pkt, buf, len);
            struct mesh_header *ph = (struct mesh_header *)pkt;
            struct aodv_rrep *prp = (struct aodv_rrep *)(pkt + sizeof(*ph));
            prp->hop_count = rp->hop_count + 1;
            memcpy(prp->relay_id, s_aodv.config.node_id, AKIRA_MESH_NODE_ID_LEN);
            LOG_INF("AODV: RREP relay replier=%02x heading_to=%02x new_hop=%u",
                    rp->target[AKIRA_MESH_NODE_ID_LEN - 1], h->dest_id[AKIRA_MESH_NODE_ID_LEN - 1],
                    prp->hop_count);
            forward_data(ph, pkt, len);
        }
    }
}

static void handle_route_reply(struct mesh_header *h, const uint8_t *buf, size_t len,
                               const uint8_t *payload, size_t plen)
{
    if (plen < sizeof(struct aodv_rrep)) {
        return;
    }
    const struct aodv_rrep *rp = (const struct aodv_rrep *)payload;
#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
    uint32_t now = k_uptime_get_32();
    uint8_t sig_buf[sizeof(struct mesh_header) + sizeof(struct aodv_rrep_sig)];
    size_t sig_len = 0;
    bool have_sig;
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    struct sign_pending *p = pending_get(s_pending_rrep, h->src_id, rp->dest_seq, now);
    have_sig = p->have_sig;
    if (!have_sig) {
        p->have_base = true;
        memcpy(p->base_buf, buf, MIN(len, sizeof(p->base_buf)));
        p->base_len = len;
    } else {
        sig_len = p->sig_len;
        memcpy(sig_buf, p->sig_buf, sig_len);
        memset(p, 0, sizeof(*p));
    }
    k_mutex_unlock(&s_aodv.lock);
    if (!have_sig) {
        return;
    }

    const struct aodv_rrep_sig *rs =
        (const struct aodv_rrep_sig *)(sig_buf + sizeof(struct mesh_header));
    if (!check_rrep_sig(rp, rs->sign_pub, rs->sig)) {
        LOG_WRN("AODV: RREP sig check failed target=%02x", rp->target[AKIRA_MESH_NODE_ID_LEN - 1]);
        return;
    }
    struct mesh_header *sh = (struct mesh_header *)sig_buf;
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    mesh_seen_add(&s_aodv.seen, sh->src_id, sh->seq_num);
    k_mutex_unlock(&s_aodv.lock);
    relay_frame(sh, sig_buf, sig_len);
#endif
    handle_route_reply_verified(h, buf, len, rp);
}

#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
static void handle_route_reply_sig(struct mesh_header *h, const uint8_t *buf, size_t len,
                                   const uint8_t *payload, size_t plen)
{
    if (plen < sizeof(struct aodv_rrep_sig)) {
        return;
    }
    const struct aodv_rrep_sig *rs = (const struct aodv_rrep_sig *)payload;
    uint32_t now = k_uptime_get_32();
    uint8_t base_buf[MESH_MAC_PACKET_BUF_SIZE];
    size_t base_len = 0;
    bool have_base;
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    struct sign_pending *p = pending_get(s_pending_rrep, h->src_id, rs->dest_seq, now);
    have_base = p->have_base;
    if (!have_base) {
        p->have_sig = true;
        memcpy(p->sig_buf, buf, MIN(len, sizeof(p->sig_buf)));
        p->sig_len = len;
    } else {
        base_len = p->base_len;
        memcpy(base_buf, p->base_buf, base_len);
        memset(p, 0, sizeof(*p));
    }
    k_mutex_unlock(&s_aodv.lock);
    if (!have_base) {
        return;
    }

    struct mesh_header *bh = (struct mesh_header *)base_buf;
    const struct aodv_rrep *rp = (const struct aodv_rrep *)(base_buf + sizeof(struct mesh_header));
    if (!check_rrep_sig(rp, rs->sign_pub, rs->sig)) {
        LOG_WRN("AODV: RREP sig check failed target=%02x", rp->target[AKIRA_MESH_NODE_ID_LEN - 1]);
        return;
    }
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    mesh_seen_add(&s_aodv.seen, h->src_id, h->seq_num);
    k_mutex_unlock(&s_aodv.lock);
    relay_frame(h, buf, len);
    handle_route_reply_verified(bh, base_buf, base_len, rp);
}
#endif

static void handle_route_error(const uint8_t *payload, size_t plen)
{
    if (plen < sizeof(struct aodv_rerr)) {
        return;
    }
    const struct aodv_rerr *re = (const struct aodv_rerr *)payload;
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    bool had_route = mesh_route_invalidate(&s_aodv.routes, re->unreachable);
    k_mutex_unlock(&s_aodv.lock);
    LOG_INF("AODV: RERR rx unreachable=%02x %s", re->unreachable[AKIRA_MESH_NODE_ID_LEN - 1],
            had_route ? "(had route, propagating)" : "(no route, dropped)");
    if (had_route) {
        /* send_rerr_broadcast is one-hop (ttl=1) by design — propagate
         * further the same way: re-broadcast from here so *this* node's own
         * one-hop neighbors hear it too, chaining back toward whoever
         * actually has routes depending on the dead link. had_route is the
         * gate that makes this self-terminating without a precursor list —
         * a node with no route to invalidate (already invalid, or never had
         * one) has nothing to tell its neighbors and stops the chain. */
        send_rerr_broadcast(re->unreachable);
    }
}

static void aodv_handle_control_frame(const uint8_t *buf, size_t len)
{
    if (len < sizeof(struct mesh_header)) {
        return;
    }
    struct mesh_header *h = (struct mesh_header *)buf;
    const uint8_t *payload = buf + sizeof(*h);
    size_t plen = len - sizeof(*h);

    /* Link-drop simulation (see akira_mesh_debug_link_drop). RREQ/RREP keep
     * the true originator's id in h->src_id across every relay hop (needed
     * for dedup/route-install correctness elsewhere) — filtering on that
     * here would also block a legitimately-relayed copy, defeating the
     * whole point. relay_id (payload) is rewritten by every hop, so it's
     * the field that actually reflects "who transmitted this specific
     * frame." RERR has no relay_id — each re-broadcast is a fresh frame
     * with h->src_id already set to the propagating node, so src_id is
     * correct there. */
    const uint8_t *immediate_sender = h->src_id;
    if (h->msg_type == AKIRA_MESH_MSG_ROUTE_REQ && plen >= sizeof(struct aodv_rreq)) {
        immediate_sender = ((const struct aodv_rreq *)payload)->relay_id;
    } else if (h->msg_type == AKIRA_MESH_MSG_ROUTE_REPLY && plen >= sizeof(struct aodv_rrep)) {
        immediate_sender = ((const struct aodv_rrep *)payload)->relay_id;
    }
    if (akira_mesh_debug_link_is_dropped(immediate_sender)) {
        return;
    }

    /* Without this, a duplicate copy of the same RREQ/RREP reaching a relay
     * from a redundant path gets rebroadcast again unconditionally (mesh_
     * route_install's freshness check only gates the route TABLE, not
     * whether the packet itself gets relayed further) — an unterminating
     * broadcast storm on any topology with more than one path between two
     * nodes. Keyed on (src_id, seq_num) same as every other layer's dedup;
     * separate cache instance from Transport's/AppDist's, so a numeric
     * coincidence between independent per-layer seq_num counters can't
     * cross-contaminate — this cache only ever sees RREQ/RREP/RERR frames.
     *
     * Lookup only here, not check_and_add: h->seq_num is unauthenticated
     * (never covered by RREQ/RREP_SIG's signature, which only signs
     * orig_seq/dest_seq) — a forged frame with a victim's src_id and a
     * seq_num the victim hasn't used yet would otherwise poison this cache
     * before verification ever runs, so the victim's later genuine frame
     * with that seq_num gets silently dropped as a dup. RREQ/RREP add to
     * the cache themselves once verified (or immediately if signing is
     * off, matching prior behavior — there's no verification step to wait
     * for in that config). RERR has no verification either way, so it
     * still adds immediately, right here. */
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    bool dup = mesh_seen_check(&s_aodv.seen, h->src_id, h->seq_num);
    k_mutex_unlock(&s_aodv.lock);
    if (dup) {
        return;
    }

    switch (h->msg_type) {
    case AKIRA_MESH_MSG_ROUTE_REQ:   handle_route_req(h, buf, len, payload, plen); break;
    case AKIRA_MESH_MSG_ROUTE_REPLY: handle_route_reply(h, buf, len, payload, plen); break;
    case AKIRA_MESH_MSG_ROUTE_ERROR:
        k_mutex_lock(&s_aodv.lock, K_FOREVER);
        mesh_seen_add(&s_aodv.seen, h->src_id, h->seq_num);
        k_mutex_unlock(&s_aodv.lock);
        handle_route_error(payload, plen);
        break;
#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
    case AKIRA_MESH_MSG_ROUTE_REQ_SIG:   handle_route_req_sig(h, buf, len, payload, plen); break;
    case AKIRA_MESH_MSG_ROUTE_REPLY_SIG: handle_route_reply_sig(h, buf, len, payload, plen); break;
#endif
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* mesh_router_ops_t implementation                                    */
/* ------------------------------------------------------------------ */

static int aodv_resolve(const uint8_t *dest_id, uint8_t *next_hop_out)
{
    uint32_t now = k_uptime_get_32();
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    struct route_entry *r = mesh_route_lookup(&s_aodv.routes, dest_id, now);
    bool have = (r != NULL);
    if (have) {
        memcpy(next_hop_out, r->next_hop, AKIRA_MESH_NODE_ID_LEN);
        mesh_route_touch(&s_aodv.routes, dest_id, now, now + MESH_ROUTE_LIFETIME_MS);
    }
    bool already_pending = mesh_pr_next_for_dest(&s_aodv.proutes, dest_id) != NULL;
    k_mutex_unlock(&s_aodv.lock);
    if (have) {
        return 0;
    }
    if (!already_pending) {
        /* First miss for this dest: the caller is responsible for queueing
         * its own payload via queue_pending (below) — resolve() only fires
         * the initial RREQ so repeated polling doesn't spam duplicate RREQs. */
        send_rreq(dest_id, NULL);
    }
    return -EHOSTUNREACH;
}

static int aodv_queue_pending(const uint8_t *dest, const uint8_t *payload,
                              uint16_t len, uint32_t timeout_ms, bool is_local_repair)
{
    uint32_t now = k_uptime_get_32();
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    int slot = mesh_pr_add(&s_aodv.proutes, dest, payload, len, now,
                           now + timeout_ms, is_local_repair);
    k_mutex_unlock(&s_aodv.lock);
    return slot;
}

static void aodv_notify_unreachable(const uint8_t *dest_id)
{
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    mesh_route_invalidate(&s_aodv.routes, dest_id);
    k_mutex_unlock(&s_aodv.lock);
}

static void aodv_tick(uint32_t now_ms)
{
    k_mutex_lock(&s_aodv.lock, K_FOREVER);
    mesh_route_gc(&s_aodv.routes, now_ms);
    mesh_pr_gc(&s_aodv.proutes, now_ms, pending_route_drop, NULL);
    mesh_pr_tick(&s_aodv.proutes, now_ms, send_rreq, NULL);
#if defined(CONFIG_AKIRA_MESH_RREQ_RREP_SIGNING)
    sign_pending_gc(now_ms);
#endif
    k_mutex_unlock(&s_aodv.lock);
    neighbor_gc(now_ms);
}

static int aodv_start(void)
{
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    /* Fresh ephemeral prekey each start — rotates naturally on every mesh
     * restart. Continuous-uptime rotation is a separate, unspecified knob,
     * intentionally not added here. */
    if (mesh_crypto_p256_keygen(s_aodv.prekey_priv, s_aodv.prekey_pub) != 0) {
        LOG_ERR("AkiraMesh AODV: failed to generate prekey");
        return -EIO;
    }
#endif
    s_aodv.started = true;
    k_work_schedule(&beacon_work, K_MSEC(beacon_next_delay_ms()));
    return 0;
}

static int aodv_stop(void)
{
    s_aodv.started = false;
    k_work_cancel_delayable(&beacon_work);
    return 0;
}

static const mesh_router_ops_t aodv_router_ops = {
    .start = aodv_start,
    .stop = aodv_stop,
    .resolve = aodv_resolve,
    .handle_control_frame = aodv_handle_control_frame,
    .queue_pending = aodv_queue_pending,
    .notify_unreachable = aodv_notify_unreachable,
    .tick = aodv_tick,
};

static int aodv_router_auto_register(void)
{
    return mesh_router_register("aodv", &aodv_router_ops);
}
SYS_INIT(aodv_router_auto_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
