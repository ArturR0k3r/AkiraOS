/**
 * @file mesh_transport.c
 * @brief AkiraMesh Transport layer — reliable unicast (per-packet ACK/retry,
 * dup suppression) and the selective-repeat ARQ stream mode.
 *
 * Depends on mesh_router_get_active() for route resolution — never on
 * mesh_aodv.h directly — so a future routing-algorithm swap leaves this
 * file untouched.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include "mesh_transport.h"
#include "mesh_router.h"
#include "mesh_mac.h"
#include "mesh_routing.h"
#include "lib/mem_helper.h"
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
#include "mesh_crypto.h"
#include "mesh_session.h"
#endif
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_mesh_transport, CONFIG_AKIRA_LOG_LEVEL);

static struct {
    akira_mesh_config_t config;
    akira_mesh_stats_t *stats;
    size_t mtu;
    uint16_t seq_num;
    struct seen_cache seen;
    struct ack_table acks;
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    struct session_table sessions;
    /* DATA sends stalled on a missing session (cold-path: no route+key yet)
     * — reuses the generic pending-route queue's shape (dest+bytes+deadline)
     * even though nothing here is AODV-specific; the stored bytes are
     * [msg_type][plaintext], not a wire frame, decoded back out in
     * mesh_transport_retry_pending(). */
    struct pending_route_q pending_keys;
#endif
    struct k_mutex lock;
    akira_mesh_rx_cb_t *rx_cb_ptr;
    void **rx_ctx_ptr;
    mesh_transport_ack_notify_t ack_notify_cb;
} s_transport AKIRA_BULK_BSS;

/* ---- selective-repeat ARQ stream mode: types + state (declared here so
 * mesh_transport_module_init below can initialize s_stream_tx.status_sem;
 * the send/receive logic itself lives further down the file). ---- */

struct __packed stream_data_hdr {
    uint16_t frame_index;    /* stable across retransmits — bitmap key */
    uint16_t frame_count;
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    /* Whole-message nonce/tag, identical on every frame — the message is
     * encrypted once as a single AES-256-CTR stream before chunking, so any
     * frame's ciphertext range is independently valid; the tag is only
     * checked once, after full reassembly. Carrying both on every frame
     * (rather than only frame 0/the last frame) keeps the existing uniform
     * offset = index * stride chunking untouched — no per-frame budget
     * varies with frame_index. */
    uint8_t  nonce[MESH_CRYPTO_NONCE_LEN];
    uint8_t  tag[MESH_CRYPTO_MAC_LEN];
#endif
};
struct __packed stream_status_req  { uint16_t base_index; uint16_t window; };
struct __packed stream_status_resp { uint16_t base_index; uint16_t window;
                                     /* bitmap follows, ceil(window/8) bytes */ };

/* Single in-flight stream send at a time (matches the existing app-transfer
 * single-slot convention). */
static struct {
    bool         active;
    struct k_sem status_sem;
    bool         got_resp;
    uint8_t      resp_bitmap[DIV_ROUND_UP(CONFIG_AKIRA_MESH_STREAM_WINDOW, 8)];
} s_stream_tx;

/* Single in-flight stream receive at a time. Buffers the whole transfer in
 * heap (freed on completion) with a bitmap tolerant of reordering — unlike
 * the app-chunk receiver, which assumes strict order and streams straight
 * to storage; stream mode targets small/medium payloads, not whole app
 * images, so RAM buffering is the right trade here. */
static struct {
    bool     active;
    uint8_t  origin_id[AKIRA_MESH_NODE_ID_LEN];
    uint16_t frame_count;
    uint16_t received_count;
    size_t   stride;
    size_t   total_len;      /* set once the true last frame arrives */
    uint8_t *buf;
    uint8_t *received_bitmap;
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    uint8_t  nonce[MESH_CRYPTO_NONCE_LEN];
    uint8_t  tag[MESH_CRYPTO_MAC_LEN];
#endif
} s_stream_rx;

/* CONFIG_AKIRA_MESH_STREAM_TX_GAP_MS exists so a burst doesn't monopolize a
 * genuinely shared, half-duplex airtime (LoRa/CC1121/BLE) — WiFi's own
 * 802.11 CSMA/CA already arbitrates the channel, so the extra gap there is
 * pure added latency. */
static uint32_t stream_tx_gap_ms(void)
{
    return (s_transport.config.transport == AKIRA_MESH_TRANSPORT_WIFI)
           ? 0 : CONFIG_AKIRA_MESH_STREAM_TX_GAP_MS;
}

static bool is_self(const uint8_t *id)
{
    return memcmp(id, s_transport.config.node_id, AKIRA_MESH_NODE_ID_LEN) == 0;
}

static bool is_broadcast_id(const uint8_t *id)
{
    for (int i = 0; i < AKIRA_MESH_NODE_ID_LEN; i++) {
        if (id[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static void fill_transport_header(struct mesh_header *h, uint8_t type, uint8_t ttl, const uint8_t *dest)
{
    h->version = 1;
    h->msg_type = type;
    h->ttl = ttl;
    memcpy(h->src_id, s_transport.config.node_id, AKIRA_MESH_NODE_ID_LEN);
    memcpy(h->dest_id, dest, AKIRA_MESH_NODE_ID_LEN);
    k_mutex_lock(&s_transport.lock, K_FOREVER);
    h->seq_num = s_transport.seq_num++;
    k_mutex_unlock(&s_transport.lock);
}

void mesh_transport_module_init(const akira_mesh_config_t *config,
                                akira_mesh_stats_t *stats, size_t mtu,
                                akira_mesh_rx_cb_t *rx_cb_ptr, void **rx_ctx_ptr)
{
    memcpy(&s_transport.config, config, sizeof(*config));
    s_transport.stats = stats;
    s_transport.mtu = mtu;
    s_transport.rx_cb_ptr = rx_cb_ptr;
    s_transport.rx_ctx_ptr = rx_ctx_ptr;
    s_transport.seq_num = 0;
    s_transport.ack_notify_cb = NULL;
    k_mutex_init(&s_transport.lock);
    mesh_seen_reset(&s_transport.seen);
    mesh_ack_reset(&s_transport.acks);
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    mesh_session_reset(&s_transport.sessions);
    mesh_pr_reset(&s_transport.pending_keys);
#endif
    k_sem_init(&s_stream_tx.status_sem, 0, 1);
}

void mesh_transport_register_ack_notify(mesh_transport_ack_notify_t cb)
{
    s_transport.ack_notify_cb = cb;
}

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
void mesh_transport_install_session(const uint8_t *local_id, const uint8_t *peer_id,
                                    const uint8_t *shared_secret, size_t shared_len,
                                    uint32_t now_ms, uint32_t ttl_ms)
{
    k_mutex_lock(&s_transport.lock, K_FOREVER);
    mesh_session_install(&s_transport.sessions, local_id, peer_id, shared_secret,
                         shared_len, now_ms, ttl_ms);
    k_mutex_unlock(&s_transport.lock);
}

void mesh_transport_touch_session(const uint8_t *peer_id, uint32_t now_ms)
{
    k_mutex_lock(&s_transport.lock, K_FOREVER);
    mesh_session_touch(&s_transport.sessions, peer_id,
                       now_ms + CONFIG_AKIRA_MESH_SESSION_LIFETIME_S * 1000U);
    k_mutex_unlock(&s_transport.lock);
}

void mesh_transport_retry_pending(const uint8_t *peer_id)
{
    while (1) {
        k_mutex_lock(&s_transport.lock, K_FOREVER);
        struct pending_route *e = mesh_pr_next_for_dest(&s_transport.pending_keys, peer_id);
        if (!e) { k_mutex_unlock(&s_transport.lock); break; }
        uint8_t enc[MESH_ROUTING_PAYLOAD_MAX];
        uint16_t l = e->len;
        memcpy(enc, e->payload, l);
        mesh_pr_clear_slot(e);
        k_mutex_unlock(&s_transport.lock);
        if (l < 1) {
            continue;
        }
        mesh_transport_send_reliable(peer_id, enc[0], enc + 1, l - 1);
    }
}
#endif

static void send_ack(const uint8_t *to, uint16_t seq)
{
    uint8_t pkt[sizeof(struct mesh_header)];
    struct mesh_header *h = (struct mesh_header *)pkt;
    /* Same hop budget as everything else, not a fixed ttl=1 — an ACK for a
     * DATA frame that traveled N hops needs N hops to get back, and the
     * dispatch below relays it like any other addressed-elsewhere frame. */
    fill_transport_header(h, AKIRA_MESH_MSG_ACK, s_transport.config.max_hops, to);
    h->seq_num = seq;   /* echo the acked seq */
    mesh_mac_send(MESH_MAC_PRIO_HIGH, pkt, sizeof(pkt));
}

/* Existence-gated relay with local repair, same shape as mesh_aodv.c's own
 * forward_data — DATA/STREAM_DATA forwarding gets the same "try our own
 * RREQ before giving up" benefit as AODV control-frame relaying, via the
 * generic router interface instead of AODV internals. */
static void transport_relay(struct mesh_header *h, const uint8_t *full, size_t len)
{
    if (h->ttl == 0) {
        return;
    }
    const mesh_router_ops_t *router = mesh_router_get_active();
    if (!router) {
        return;
    }
    uint8_t next_hop[AKIRA_MESH_NODE_ID_LEN];
    if (router->resolve(h->dest_id, next_hop) == 0) {
        uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
        if (len > sizeof(pkt)) {
            return;
        }
        memcpy(pkt, full, len);
        ((struct mesh_header *)pkt)->ttl = h->ttl - 1;
        if (s_transport.stats) {
            s_transport.stats->messages_forwarded++;
        }
        mesh_mac_send(MESH_MAC_PRIO_LOW, pkt, len);
        return;
    }
    if (h->ttl <= 1) {
        return; /* no budget left for a repair round trip */
    }
    router->queue_pending(h->dest_id, full, (uint16_t)len,
                          CONFIG_AKIRA_MESH_LOCAL_REPAIR_TIMEOUT_MS, true);
}

/* ------------------------------------------------------------------ */
/* Reliable unicast send                                               */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
int mesh_transport_send_reliable(const uint8_t *dest_id, uint8_t msg_type,
                                 const uint8_t *data, size_t len)
{
    const mesh_router_ops_t *router = mesh_router_get_active();
    if (!router) {
        return -ENODEV;
    }
    uint32_t now = k_uptime_get_32();
    bool need_key = (msg_type == AKIRA_MESH_MSG_DATA);

    uint8_t next_hop[AKIRA_MESH_NODE_ID_LEN];
    bool have_route = (router->resolve(dest_id, next_hop) == 0);

    k_mutex_lock(&s_transport.lock, K_FOREVER);
    struct session_entry *sess = need_key ?
        mesh_session_lookup(&s_transport.sessions, dest_id, now) : NULL;
    uint8_t enc_key[MESH_CRYPTO_SESSION_KEY_LEN];
    uint8_t mac_key[MESH_CRYPTO_MAC_KEY_LEN];
    bool have_key = !need_key;
    if (sess) {
        memcpy(enc_key, sess->enc_key, sizeof(enc_key));
        memcpy(mac_key, sess->mac_key, sizeof(mac_key));
        have_key = true;
    }
    k_mutex_unlock(&s_transport.lock);

    if (!have_route || !have_key) {
        /* Missing route and/or session key: hold the plaintext (not a wire
         * frame — there's no key to encrypt it with yet) and let discovery
         * (already triggered by resolve() above on a miss) complete both —
         * the RREP that completes route discovery also completes the key
         * handshake (mesh_aodv.c's ROUTE_REQ/ROUTE_REPLY handling), which
         * calls mesh_transport_retry_pending() to re-run this function once
         * both exist. Never send DATA unencrypted as a fallback. */
        if (len + 1 > MESH_ROUTING_PAYLOAD_MAX) {
            return -EMSGSIZE;
        }
        uint8_t enc[MESH_ROUTING_PAYLOAD_MAX];
        enc[0] = msg_type;
        memcpy(enc + 1, data, len);
        k_mutex_lock(&s_transport.lock, K_FOREVER);
        int pr = mesh_pr_add(&s_transport.pending_keys, dest_id, enc, (uint16_t)(len + 1),
                             now, now + mesh_mac_ack_timeout_ms() *
                             (CONFIG_AKIRA_MESH_MAX_RETRIES + 1), false);
        k_mutex_unlock(&s_transport.lock);
        return (pr < 0) ? -EBUSY : 0;
    }

    uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_transport_header(h, msg_type, s_transport.config.max_hops, dest_id);
    uint8_t *body = pkt + sizeof(*h);
    size_t total;

    if (need_key) {
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
        total = sizeof(*h) + MESH_CRYPTO_NONCE_LEN + len + MESH_CRYPTO_MAC_LEN;
        memset(enc_key, 0, sizeof(enc_key));
        memset(mac_key, 0, sizeof(mac_key));
        memset(tag, 0, sizeof(tag));
    } else {
        memcpy(body, data, len);
        total = sizeof(*h) + len;
    }
    uint16_t seq = h->seq_num;

    k_mutex_lock(&s_transport.lock, K_FOREVER);
    int slot = mesh_ack_add(&s_transport.acks, seq, dest_id, pkt, total, now,
                            now + mesh_mac_ack_timeout_ms());
    k_mutex_unlock(&s_transport.lock);
    if (slot < 0) {
        return -EBUSY;
    }
    if (s_transport.stats) {
        s_transport.stats->messages_sent++;
    }
    return mesh_mac_send(MESH_MAC_PRIO_LOW, pkt, total);
}
#else /* !CONFIG_AKIRA_MESH_E2E_CRYPTO */
int mesh_transport_send_reliable(const uint8_t *dest_id, uint8_t msg_type,
                                 const uint8_t *data, size_t len)
{
    const mesh_router_ops_t *router = mesh_router_get_active();
    if (!router) {
        return -ENODEV;
    }
    uint32_t now = k_uptime_get_32();
    uint8_t next_hop[AKIRA_MESH_NODE_ID_LEN];
    bool have_route = (router->resolve(dest_id, next_hop) == 0);

    uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_transport_header(h, msg_type, s_transport.config.max_hops, dest_id);
    memcpy(pkt + sizeof(*h), data, len);
    size_t total = sizeof(*h) + len;
    uint16_t seq = h->seq_num;

    if (have_route) {
        k_mutex_lock(&s_transport.lock, K_FOREVER);
        int slot = mesh_ack_add(&s_transport.acks, seq, dest_id, pkt, total, now,
                                now + mesh_mac_ack_timeout_ms());
        k_mutex_unlock(&s_transport.lock);
        if (slot < 0) {
            return -EBUSY;
        }
        if (s_transport.stats) {
            s_transport.stats->messages_sent++;
        }
        return mesh_mac_send(MESH_MAC_PRIO_LOW, pkt, total);
    }

    int pr = router->queue_pending(dest_id, pkt, (uint16_t)total,
                                   mesh_mac_ack_timeout_ms() *
                                   (CONFIG_AKIRA_MESH_MAX_RETRIES + 1), false);
    return (pr < 0) ? -EBUSY : 0;
}
#endif /* CONFIG_AKIRA_MESH_E2E_CRYPTO */

/* ------------------------------------------------------------------ */
/* Fire-and-forget unicast send                                        */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
int mesh_transport_send_unreliable(const uint8_t *dest_id, const uint8_t *data, size_t len)
{
    const mesh_router_ops_t *router = mesh_router_get_active();
    if (!router) {
        return -ENODEV;
    }
    uint32_t now = k_uptime_get_32();
    uint8_t next_hop[AKIRA_MESH_NODE_ID_LEN];
    if (router->resolve(dest_id, next_hop) != 0) {
        return -EHOSTUNREACH;  /* fail fast, unlike DATA's queue-and-wait */
    }

    k_mutex_lock(&s_transport.lock, K_FOREVER);
    struct session_entry *sess = mesh_session_lookup(&s_transport.sessions, dest_id, now);
    uint8_t enc_key[MESH_CRYPTO_SESSION_KEY_LEN];
    uint8_t mac_key[MESH_CRYPTO_MAC_KEY_LEN];
    bool have_key = (sess != NULL);
    if (have_key) {
        memcpy(enc_key, sess->enc_key, sizeof(enc_key));
        memcpy(mac_key, sess->mac_key, sizeof(mac_key));
    }
    k_mutex_unlock(&s_transport.lock);
    if (!have_key) {
        return -ENOTCONN;  /* fail fast, unlike DATA's queue-and-wait */
    }

    uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_transport_header(h, AKIRA_MESH_MSG_DATA_UNRELIABLE, s_transport.config.max_hops, dest_id);
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
    mesh_crypto_hmac_sha256(mac_key, sizeof(mac_key), body, MESH_CRYPTO_NONCE_LEN + len, tag);
    memcpy(ct + len, tag, MESH_CRYPTO_MAC_LEN);
    size_t total = sizeof(*h) + MESH_CRYPTO_NONCE_LEN + len + MESH_CRYPTO_MAC_LEN;
    memset(enc_key, 0, sizeof(enc_key));
    memset(mac_key, 0, sizeof(mac_key));
    memset(tag, 0, sizeof(tag));

    if (s_transport.stats) {
        s_transport.stats->messages_sent++;
    }
    return mesh_mac_send(MESH_MAC_PRIO_LOW, pkt, total);
}
#else /* !CONFIG_AKIRA_MESH_E2E_CRYPTO */
int mesh_transport_send_unreliable(const uint8_t *dest_id, const uint8_t *data, size_t len)
{
    const mesh_router_ops_t *router = mesh_router_get_active();
    if (!router) {
        return -ENODEV;
    }
    uint8_t next_hop[AKIRA_MESH_NODE_ID_LEN];
    if (router->resolve(dest_id, next_hop) != 0) {
        return -EHOSTUNREACH;
    }
    uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_transport_header(h, AKIRA_MESH_MSG_DATA_UNRELIABLE, s_transport.config.max_hops, dest_id);
    memcpy(pkt + sizeof(*h), data, len);
    size_t total = sizeof(*h) + len;
    if (s_transport.stats) {
        s_transport.stats->messages_sent++;
    }
    return mesh_mac_send(MESH_MAC_PRIO_LOW, pkt, total);
}
#endif /* CONFIG_AKIRA_MESH_E2E_CRYPTO */

/* ------------------------------------------------------------------ */
/* DATA/ACK dispatch                                                   */
/* ------------------------------------------------------------------ */

static void handle_data(struct mesh_header *h, const uint8_t *buf, size_t len,
                        const uint8_t *payload, size_t plen, uint32_t now, bool reliable)
{
#if !defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    ARG_UNUSED(reliable);
#endif
    if (is_self(h->dest_id)) {
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
        if (plen < MESH_CRYPTO_NONCE_LEN + MESH_CRYPTO_MAC_LEN) {
            return;
        }
        const uint8_t *nonce = payload;
        const uint8_t *ct = payload + MESH_CRYPTO_NONCE_LEN;
        size_t ct_len = plen - MESH_CRYPTO_NONCE_LEN - MESH_CRYPTO_MAC_LEN;
        const uint8_t *tag = ct + ct_len;

        k_mutex_lock(&s_transport.lock, K_FOREVER);
        struct session_entry *sess = mesh_session_lookup(&s_transport.sessions, h->src_id, now);
        uint8_t enc_key[MESH_CRYPTO_SESSION_KEY_LEN];
        uint8_t mac_key[MESH_CRYPTO_MAC_KEY_LEN];
        bool have_sess = (sess != NULL);
        if (have_sess) {
            memcpy(enc_key, sess->enc_key, sizeof(enc_key));
            memcpy(mac_key, sess->mac_key, sizeof(mac_key));
        }
        k_mutex_unlock(&s_transport.lock);

        if (!have_sess) {
            LOG_WRN("AkiraMesh: DATA from %02x%02x with no cached session, dropped",
                    h->src_id[0], h->src_id[1]);
            return;
        }

        uint8_t expect_tag[32];
        mesh_crypto_hmac_sha256(mac_key, sizeof(mac_key), payload,
                                MESH_CRYPTO_NONCE_LEN + ct_len, expect_tag);
        bool tag_ok = mesh_crypto_const_time_eq(expect_tag, tag, MESH_CRYPTO_MAC_LEN);
        if (!tag_ok) {
            LOG_WRN("AkiraMesh: DATA MAC mismatch from %02x%02x, dropped "
                   "(plen=%zu ct_len=%zu seq=%u)",
                   h->src_id[0], h->src_id[1], plen, ct_len, h->seq_num);
            memset(enc_key, 0, sizeof(enc_key));
            memset(mac_key, 0, sizeof(mac_key));
            memset(expect_tag, 0, sizeof(expect_tag));
            return;
        }

        uint8_t plaintext[MESH_MAC_PACKET_BUF_SIZE];
        mesh_crypto_aes256_ctr(enc_key, nonce, ct, ct_len, plaintext);
        memset(enc_key, 0, sizeof(enc_key));
        memset(mac_key, 0, sizeof(mac_key));
        memset(expect_tag, 0, sizeof(expect_tag));

        k_mutex_lock(&s_transport.lock, K_FOREVER);
        bool dup = mesh_seen_check_and_add(&s_transport.seen, h->src_id, h->seq_num);
        k_mutex_unlock(&s_transport.lock);
        if (reliable) {
            send_ack(h->src_id, h->seq_num);
        }
        if (!dup && s_transport.rx_cb_ptr && *s_transport.rx_cb_ptr) {
            (*s_transport.rx_cb_ptr)(h->src_id, plaintext, ct_len, *s_transport.rx_ctx_ptr);
        }
        memset(plaintext, 0, sizeof(plaintext));
#else
        LOG_WRN("AkiraMesh: DATA received but E2E crypto not compiled in, dropped");
#endif
    } else if (is_broadcast_id(h->dest_id)) {
        if (s_transport.rx_cb_ptr && *s_transport.rx_cb_ptr) {
            (*s_transport.rx_cb_ptr)(h->src_id, payload, plen, *s_transport.rx_ctx_ptr);
        }
        if (h->ttl > 1) {
            uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
            if (len <= sizeof(pkt)) {
                memcpy(pkt, buf, len);
                ((struct mesh_header *)pkt)->ttl = h->ttl - 1;
                if (s_transport.stats) {
                    s_transport.stats->messages_forwarded++;
                }
                mesh_mac_send(MESH_MAC_PRIO_LOW, pkt, len);
            }
        }
    } else {
        transport_relay(h, buf, len);
    }
}

/* ------------------------------------------------------------------ */
/* Selective-repeat ARQ stream mode                                    */
/* ------------------------------------------------------------------ */

static void stream_rx_reset(void)
{
    if (s_stream_rx.buf) {
        akira_free_buffer(s_stream_rx.buf);
    }
    if (s_stream_rx.received_bitmap) {
        akira_free_buffer(s_stream_rx.received_bitmap);
    }
    memset(&s_stream_rx, 0, sizeof(s_stream_rx));
}

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
static void handle_stream_data_frame(const struct mesh_header *h, const uint8_t *payload,
                                     size_t plen, uint32_t now)
{
    if (plen < sizeof(struct stream_data_hdr)) {
        return;
    }
    const struct stream_data_hdr *sh = (const struct stream_data_hdr *)payload;
    const uint8_t *data = payload + sizeof(*sh);
    size_t data_len = plen - sizeof(*sh);
    if (sh->frame_count == 0) {
        return;
    }

    bool new_transfer = !s_stream_rx.active ||
                        memcmp(s_stream_rx.origin_id, h->src_id, AKIRA_MESH_NODE_ID_LEN) != 0 ||
                        s_stream_rx.frame_count != sh->frame_count;
    if (new_transfer) {
        /* Frame 0 specifically defines the stride, not whichever frame
         * happens to arrive first — the sender's chunking (chunk_len =
         * MIN(stride, ct_len - off)) only guarantees a full-stride frame at
         * index 0; the last frame is typically shorter. Radio delivery has
         * no ordering guarantee (CSMA requeue-on-busy can reorder frames in
         * the same lane too), so if the last (short) frame arrives first,
         * inferring stride from it truncates every later full-length frame.
         * Drop non-zero-index frames for a transfer we haven't started yet
         * — the selective-repeat retry (stream_query_and_retransmit) picks
         * them back up once frame 0 has been seen. */
        if (sh->frame_index != 0) {
            return;
        }
        stream_rx_reset();
        size_t stride_guess = data_len;
        if (stride_guess == 0) {
            return;
        }
        s_stream_rx.buf = akira_malloc_buffer((size_t)sh->frame_count * stride_guess);
        s_stream_rx.received_bitmap = akira_malloc_buffer(DIV_ROUND_UP(sh->frame_count, 8));
        if (!s_stream_rx.buf || !s_stream_rx.received_bitmap) {
            stream_rx_reset();
            return;
        }
        s_stream_rx.active = true;
        memcpy(s_stream_rx.origin_id, h->src_id, AKIRA_MESH_NODE_ID_LEN);
        s_stream_rx.frame_count = sh->frame_count;
        s_stream_rx.stride = stride_guess;
    }

    /* Keep the session alive for the duration of the transfer — a long
     * stream on a slow link can outlive CONFIG_AKIRA_MESH_SESSION_LIFETIME_S
     * if nothing refreshes it, and the key itself doesn't need re-deriving,
     * only the table entry needs to not expire before the final decrypt. */
    mesh_transport_touch_session(h->src_id, now);
    memcpy(s_stream_rx.nonce, sh->nonce, MESH_CRYPTO_NONCE_LEN);
    memcpy(s_stream_rx.tag, sh->tag, MESH_CRYPTO_MAC_LEN);

    if (sh->frame_index >= s_stream_rx.frame_count) {
        return;
    }
    size_t bit = sh->frame_index;
    bool already = (s_stream_rx.received_bitmap[bit / 8] & BIT(bit % 8)) != 0;
    if (!already) {
        size_t off = (size_t)sh->frame_index * s_stream_rx.stride;
        size_t copy_len = MIN(data_len, s_stream_rx.stride);
        memcpy(s_stream_rx.buf + off, data, copy_len);
        s_stream_rx.received_bitmap[bit / 8] |= BIT(bit % 8);
        s_stream_rx.received_count++;
        if (sh->frame_index == s_stream_rx.frame_count - 1) {
            s_stream_rx.total_len = off + data_len;
        }
    }

    if (s_stream_rx.received_count == s_stream_rx.frame_count) {
        k_mutex_lock(&s_transport.lock, K_FOREVER);
        struct session_entry *sess = mesh_session_lookup(&s_transport.sessions,
                                                          s_stream_rx.origin_id, now);
        uint8_t enc_key[MESH_CRYPTO_SESSION_KEY_LEN];
        uint8_t mac_key[MESH_CRYPTO_MAC_KEY_LEN];
        bool have_key = (sess != NULL);
        if (have_key) {
            memcpy(enc_key, sess->enc_key, sizeof(enc_key));
            memcpy(mac_key, sess->mac_key, sizeof(mac_key));
        }
        k_mutex_unlock(&s_transport.lock);

        bool ok = false;
        if (have_key) {
            uint8_t *tagbuf = akira_malloc_buffer(MESH_CRYPTO_NONCE_LEN + s_stream_rx.total_len);
            if (tagbuf) {
                memcpy(tagbuf, s_stream_rx.nonce, MESH_CRYPTO_NONCE_LEN);
                memcpy(tagbuf + MESH_CRYPTO_NONCE_LEN, s_stream_rx.buf, s_stream_rx.total_len);
                uint8_t expect[32];
                mesh_crypto_hmac_sha256(mac_key, sizeof(mac_key), tagbuf,
                                        MESH_CRYPTO_NONCE_LEN + s_stream_rx.total_len, expect);
                ok = mesh_crypto_const_time_eq(expect, s_stream_rx.tag, MESH_CRYPTO_MAC_LEN);
                akira_free_buffer(tagbuf);
                memset(expect, 0, sizeof(expect));
            }
        }
        if (ok) {
            /* AES-CTR is its own inverse — decrypt in place. */
            mesh_crypto_aes256_ctr(enc_key, s_stream_rx.nonce, s_stream_rx.buf,
                                   s_stream_rx.total_len, s_stream_rx.buf);
            if (s_transport.rx_cb_ptr && *s_transport.rx_cb_ptr) {
                (*s_transport.rx_cb_ptr)(s_stream_rx.origin_id, s_stream_rx.buf,
                                         s_stream_rx.total_len, *s_transport.rx_ctx_ptr);
            }
        } else {
            LOG_WRN("AkiraMesh: stream from %02x%02x failed auth (no session or tag mismatch), dropped",
                    h->src_id[0], h->src_id[1]);
        }
        memset(enc_key, 0, sizeof(enc_key));
        memset(mac_key, 0, sizeof(mac_key));
        stream_rx_reset();
    }
}
#else /* !CONFIG_AKIRA_MESH_E2E_CRYPTO */
static void handle_stream_data_frame(const struct mesh_header *h, const uint8_t *payload,
                                     size_t plen, uint32_t now)
{
    ARG_UNUSED(h); ARG_UNUSED(payload); ARG_UNUSED(plen); ARG_UNUSED(now);
    /* Stream mode requires E2E crypto (encrypt-once-then-chunk-ciphertext) —
     * akira_mesh_send_stream() returns -ENOTSUP without it, so a compliant
     * peer never sends this; drop defensively if one arrives anyway. */
}
#endif

static void reply_stream_status(const uint8_t *to, const uint8_t *payload, size_t plen)
{
    if (plen < sizeof(struct stream_status_req)) {
        return;
    }
    const struct stream_status_req *req = (const struct stream_status_req *)payload;
    uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_transport_header(h, AKIRA_MESH_MSG_STREAM_STATUS_RESP, 1, to);
    struct stream_status_resp *resp = (struct stream_status_resp *)(pkt + sizeof(*h));
    resp->base_index = req->base_index;
    resp->window = req->window;
    uint8_t *bitmap_out = (uint8_t *)resp + sizeof(*resp);
    size_t max_bitmap = sizeof(pkt) - sizeof(*h) - sizeof(*resp);
    size_t bitmap_bytes = MIN(DIV_ROUND_UP(req->window, 8), max_bitmap);
    memset(bitmap_out, 0, bitmap_bytes);
    if (s_stream_rx.active) {
        for (uint16_t i = 0; i < req->window && (req->base_index + i) < s_stream_rx.frame_count; i++) {
            size_t bit = req->base_index + i;
            if (s_stream_rx.received_bitmap[bit / 8] & BIT(bit % 8)) {
                bitmap_out[i / 8] |= BIT(i % 8);
            }
        }
    }
    mesh_mac_send(MESH_MAC_PRIO_MEDIUM, pkt, sizeof(*h) + sizeof(*resp) + bitmap_bytes);
}

static void handle_stream_status_resp(const uint8_t *payload, size_t plen)
{
    if (plen < sizeof(struct stream_status_resp)) {
        return;
    }
    const struct stream_status_resp *resp = (const struct stream_status_resp *)payload;
    if (!s_stream_tx.active) {
        return;
    }
    size_t bitmap_len = plen - sizeof(*resp);
    size_t n = MIN(bitmap_len, sizeof(s_stream_tx.resp_bitmap));
    memcpy(s_stream_tx.resp_bitmap, (const uint8_t *)resp + sizeof(*resp), n);
    if (n < sizeof(s_stream_tx.resp_bitmap)) {
        memset(s_stream_tx.resp_bitmap + n, 0, sizeof(s_stream_tx.resp_bitmap) - n);
    }
    s_stream_tx.got_resp = true;
    k_sem_give(&s_stream_tx.status_sem);
}

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
/* Builds one STREAM_DATA frame carrying ciphertext bytes [idx*stride,
 * idx*stride+chunk_len) plus the whole-message nonce/tag — shared by the
 * initial burst and the gap-fill retransmit loop below so the framing logic
 * (and the crypto fields riding on it) exists in exactly one place. */
static size_t build_stream_frame(uint8_t *pkt, const uint8_t *dest_id, uint8_t ttl,
                                 uint16_t idx, uint16_t frame_count,
                                 const uint8_t *ct, size_t ct_len, size_t stride,
                                 const uint8_t *nonce, const uint8_t *tag)
{
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_transport_header(h, AKIRA_MESH_MSG_STREAM_DATA, ttl, dest_id);
    struct stream_data_hdr *sh = (struct stream_data_hdr *)(pkt + sizeof(*h));
    sh->frame_index = idx;
    sh->frame_count = frame_count;
    memcpy(sh->nonce, nonce, MESH_CRYPTO_NONCE_LEN);
    memcpy(sh->tag, tag, MESH_CRYPTO_MAC_LEN);
    size_t off = (size_t)idx * stride;
    size_t chunk_len = MIN(stride, ct_len - off);
    memcpy(pkt + sizeof(*h) + sizeof(*sh), ct + off, chunk_len);
    return sizeof(*h) + sizeof(*sh) + chunk_len;
}

/* Query which indices in [base_index, base_index+window) landed, and
 * retransmit only the gaps. Returns -ETIMEDOUT if the receiver never
 * answered (caller decides whether to retry the whole window). */
static int stream_query_and_retransmit(const uint8_t *dest_id, const uint8_t *ct,
                                       size_t ct_len, size_t stride, uint16_t frame_count,
                                       uint16_t base_index, uint16_t window,
                                       const uint8_t *nonce, const uint8_t *tag)
{
    uint8_t pkt[sizeof(struct mesh_header) + sizeof(struct stream_status_req)];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_transport_header(h, AKIRA_MESH_MSG_STREAM_STATUS_REQ, s_transport.config.max_hops, dest_id);
    struct stream_status_req *req = (struct stream_status_req *)(pkt + sizeof(*h));
    req->base_index = base_index;
    req->window = window;

    int attempt;
    for (attempt = 0; attempt <= CONFIG_AKIRA_MESH_MAX_RETRIES; attempt++) {
        s_stream_tx.got_resp = false;
        k_sem_reset(&s_stream_tx.status_sem);
        int64_t t0 = k_uptime_get();
        mesh_mac_send(MESH_MAC_PRIO_MEDIUM, pkt, sizeof(pkt));
        int wait_ret = k_sem_take(&s_stream_tx.status_sem, K_MSEC(mesh_mac_ack_timeout_ms()));
        LOG_INF("stream status: base=%u window=%u attempt=%d wait=%lldms got_resp=%d",
               base_index, window, attempt, k_uptime_get() - t0, s_stream_tx.got_resp);
        if (wait_ret == 0 && s_stream_tx.got_resp) {
            break;
        }
    }
    if (attempt > CONFIG_AKIRA_MESH_MAX_RETRIES) {
        return -ETIMEDOUT;
    }

    for (uint16_t i = 0; i < window; i++) {
        uint16_t idx = base_index + i;
        if (idx >= frame_count) {
            break;
        }
        if (s_stream_tx.resp_bitmap[i / 8] & BIT(i % 8)) {
            continue; /* already landed */
        }
        uint8_t fpkt[MESH_MAC_PACKET_BUF_SIZE];
        size_t plen = build_stream_frame(fpkt, dest_id, s_transport.config.max_hops,
                                         idx, frame_count, ct, ct_len, stride, nonce, tag);
        mesh_mac_send(MESH_MAC_PRIO_LOW, fpkt, plen);
        k_msleep(stream_tx_gap_ms());
    }
    return 0;
}

int akira_mesh_send_stream(const uint8_t *dest_id, const uint8_t *data, size_t len)
{
    if (!dest_id || !data || len == 0) {
        return -EINVAL;
    }
    const mesh_router_ops_t *router = mesh_router_get_active();
    if (!router) {
        return -ENODEV;
    }
    if (s_stream_tx.active) {
        return -EBUSY;
    }

    if (s_transport.mtu <= sizeof(struct mesh_header) + sizeof(struct stream_data_hdr)) {
        return -EMSGSIZE;
    }
    size_t stride = s_transport.mtu - sizeof(struct mesh_header) - sizeof(struct stream_data_hdr);
    uint16_t frame_count = (uint16_t)DIV_ROUND_UP(len, stride);

    /* Cold destination: resolve()'s side effect fires an RREQ on first miss
     * (aodv_resolve dedups repeat calls while one is in flight), and the
     * RREP that installs the route also derives the E2E session in the same
     * handler (handle_route_reply) — so both come ready together. Poll both
     * up to the same total budget mesh_transport_send_reliable's cold path
     * uses for its own queued retry window. */
    uint32_t discovery_deadline = k_uptime_get_32() +
        mesh_mac_ack_timeout_ms() * (CONFIG_AKIRA_MESH_MAX_RETRIES + 1);

    uint8_t next_hop[AKIRA_MESH_NODE_ID_LEN];
    if (router->resolve(dest_id, next_hop) != 0) {
        /* resolve()'s own RREQ-dedup is keyed off the router's pending-route
         * queue (aodv_resolve: "caller is responsible for queueing its own
         * payload via queue_pending") — without registering here, polling
         * resolve() below would refire a fresh RREQ every poll interval
         * instead of waiting on the one already in flight. */
        router->queue_pending(dest_id, dest_id, 0,
                              discovery_deadline - k_uptime_get_32(), false);
        while (router->resolve(dest_id, next_hop) != 0) {
            if ((int32_t)(k_uptime_get_32() - discovery_deadline) >= 0) {
                return -EHOSTUNREACH;
            }
            k_msleep(CONFIG_AKIRA_MESH_ROUTE_DISCOVERY_POLL_MS);
        }
    }

    uint8_t enc_key[MESH_CRYPTO_SESSION_KEY_LEN];
    uint8_t mac_key[MESH_CRYPTO_MAC_KEY_LEN];
    for (;;) {
        uint32_t now = k_uptime_get_32();
        k_mutex_lock(&s_transport.lock, K_FOREVER);
        struct session_entry *sess = mesh_session_lookup(&s_transport.sessions, dest_id, now);
        bool have_key = (sess != NULL);
        if (have_key) {
            memcpy(enc_key, sess->enc_key, sizeof(enc_key));
            memcpy(mac_key, sess->mac_key, sizeof(mac_key));
        }
        k_mutex_unlock(&s_transport.lock);
        if (have_key) {
            break;
        }
        if ((int32_t)(k_uptime_get_32() - discovery_deadline) >= 0) {
            return -EACCES;
        }
        k_msleep(CONFIG_AKIRA_MESH_ROUTE_DISCOVERY_POLL_MS);
    }

    /* Whole message encrypted once as a single AES-256-CTR stream *before*
     * chunking, then ciphertext bytes are chunked exactly like plaintext —
     * any frame's ciphertext range decrypts independently once the whole
     * blob is reassembled, so per-frame encryption (and the counter-offset
     * math that would require) is never needed. nonce lives
     * in the first NONCE_LEN bytes of buf, ciphertext right after it, so the
     * tag (computed once, below) can cover nonce||ciphertext contiguously —
     * same construction mesh_transport_send_reliable() uses for plain DATA. */
    uint8_t *ebuf = akira_malloc_buffer(MESH_CRYPTO_NONCE_LEN + len);
    if (!ebuf) {
        memset(enc_key, 0, sizeof(enc_key));
        memset(mac_key, 0, sizeof(mac_key));
        return -ENOMEM;
    }
    uint8_t *nonce = ebuf;
    uint8_t *ct = ebuf + MESH_CRYPTO_NONCE_LEN;
    sys_csrand_get(nonce, MESH_CRYPTO_NONCE_LEN);
    if (mesh_crypto_aes256_ctr(enc_key, nonce, data, len, ct) != 0) {
        akira_free_buffer(ebuf);
        memset(enc_key, 0, sizeof(enc_key));
        memset(mac_key, 0, sizeof(mac_key));
        return -EIO;
    }
    uint8_t taghash[32];
    mesh_crypto_hmac_sha256(mac_key, sizeof(mac_key), ebuf, MESH_CRYPTO_NONCE_LEN + len, taghash);
    uint8_t tag[MESH_CRYPTO_MAC_LEN];
    memcpy(tag, taghash, MESH_CRYPTO_MAC_LEN);
    memset(enc_key, 0, sizeof(enc_key));
    memset(mac_key, 0, sizeof(mac_key));
    memset(taghash, 0, sizeof(taghash));

    s_stream_tx.active = true;
    uint16_t base_index = 0;
    int ret = 0;
    while (base_index < frame_count) {
        uint16_t window = MIN((uint16_t)CONFIG_AKIRA_MESH_STREAM_WINDOW,
                              (uint16_t)(frame_count - base_index));
        for (uint16_t i = 0; i < window; i++) {
            uint16_t idx = base_index + i;
            uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
            size_t plen = build_stream_frame(pkt, dest_id, s_transport.config.max_hops,
                                             idx, frame_count, ct, len, stride, nonce, tag);
            mesh_mac_send(MESH_MAC_PRIO_LOW, pkt, plen);
            k_msleep(stream_tx_gap_ms());
            if ((i + 1) % CONFIG_AKIRA_MESH_STREAM_STATUS_INTERVAL == 0) {
                stream_query_and_retransmit(dest_id, ct, len, stride, frame_count,
                                            base_index, window, nonce, tag);
            }
        }
        /* Final check for this window even if it didn't land on a
         * STATUS_INTERVAL boundary. */
        if (stream_query_and_retransmit(dest_id, ct, len, stride, frame_count,
                                        base_index, window, nonce, tag) != 0) {
            ret = -ETIMEDOUT;
            break;
        }
        base_index += window;
    }
    akira_free_buffer(ebuf);
    s_stream_tx.active = false;
    return ret;
}
#else /* !CONFIG_AKIRA_MESH_E2E_CRYPTO */
int akira_mesh_send_stream(const uint8_t *dest_id, const uint8_t *data, size_t len)
{
    ARG_UNUSED(dest_id); ARG_UNUSED(data); ARG_UNUSED(len);
    return -ENOTSUP;
}
#endif

/* ------------------------------------------------------------------ */
/* RX dispatch entry point                                             */
/* ------------------------------------------------------------------ */

void mesh_transport_handle_frame(const uint8_t *buf, size_t len)
{
    if (len < sizeof(struct mesh_header)) {
        return;
    }
    struct mesh_header *h = (struct mesh_header *)buf;
    uint32_t now = k_uptime_get_32();

    /* Duplicate suppression skipped for frames addressed to us where the
     * payload's own idempotency check is finer-grained than a bare seq_num
     * (DATA re-validates via AEAD tag, stream frames re-mark the same
     * bitmap bit) — a "seen" bit here would permanently swallow a clean
     * retry behind e.g. a corrupted first attempt. Frames we relay (dest
     * != us) are still deduped: a relay doesn't validate content, so
     * seen-based loop/flood suppression is both safe and wanted there. */
    bool self_dest = is_self(h->dest_id);
    /* ACK now relays past 1 hop like everything else (see send_ack) — only
     * skip dedup for it on the self_dest side, same as DATA/STREAM_*;
     * a relayed (!self_dest) ACK still needs loop/flood suppression. */
    bool skip_dedup = self_dest && (h->msg_type == AKIRA_MESH_MSG_ACK ||
                                    h->msg_type == AKIRA_MESH_MSG_DATA ||
                                    h->msg_type == AKIRA_MESH_MSG_DATA_UNRELIABLE ||
                                    h->msg_type == AKIRA_MESH_MSG_STREAM_DATA ||
                                    h->msg_type == AKIRA_MESH_MSG_STREAM_STATUS_REQ ||
                                    h->msg_type == AKIRA_MESH_MSG_STREAM_STATUS_RESP);
    if (!skip_dedup) {
        k_mutex_lock(&s_transport.lock, K_FOREVER);
        bool dup = mesh_seen_check_and_add(&s_transport.seen, h->src_id, h->seq_num);
        k_mutex_unlock(&s_transport.lock);
        if (dup) {
            return;
        }
    }

    const uint8_t *payload = buf + sizeof(*h);
    size_t plen = len - sizeof(*h);

    switch (h->msg_type) {
    case AKIRA_MESH_MSG_DATA:
        handle_data(h, buf, len, payload, plen, now, true);
        break;
    case AKIRA_MESH_MSG_DATA_UNRELIABLE:
        handle_data(h, buf, len, payload, plen, now, false);
        break;
    case AKIRA_MESH_MSG_ACK:
        if (self_dest) {
            uint32_t elapsed_ms = 0;
            uint8_t retries = 0;
            k_mutex_lock(&s_transport.lock, K_FOREVER);
            bool cleared = mesh_ack_clear(&s_transport.acks, h->seq_num, h->src_id,
                                          now, &elapsed_ms, &retries);
            k_mutex_unlock(&s_transport.lock);
            if (cleared) {
                LOG_INF("DATA seq=%u delivered in %ums (retries=%u)",
                       h->seq_num, elapsed_ms, retries);
            }
            if (s_transport.ack_notify_cb) {
                s_transport.ack_notify_cb(h->seq_num, h->src_id, false);
            }
        } else {
            /* Not ours — an overhearing node on a shared-medium radio must
             * not clear its own pending-ACK entry just because (seq_num,
             * src_id) happens to collide with someone else's independent
             * per-node counter; relay toward the real dest_id instead. */
            transport_relay(h, buf, len);
        }
        break;
    case AKIRA_MESH_MSG_STREAM_DATA:
        if (self_dest) {
            handle_stream_data_frame(h, payload, plen, now);
        } else {
            transport_relay(h, buf, len);
        }
        break;
    case AKIRA_MESH_MSG_STREAM_STATUS_REQ:
        if (self_dest) {
            reply_stream_status(h->src_id, payload, plen);
        } else {
            transport_relay(h, buf, len);
        }
        break;
    case AKIRA_MESH_MSG_STREAM_STATUS_RESP:
        if (self_dest) {
            handle_stream_status_resp(payload, plen);
        } else {
            transport_relay(h, buf, len);
        }
        break;
    default:
        break;
    }
}

void mesh_transport_tick(uint32_t now_ms)
{
    const mesh_router_ops_t *router = mesh_router_get_active();
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
    k_mutex_lock(&s_transport.lock, K_FOREVER);
    mesh_pr_gc(&s_transport.pending_keys, now_ms, NULL, NULL);
    k_mutex_unlock(&s_transport.lock);
#endif
    for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_PENDING_ACKS; i++) {
        uint8_t buf[MESH_MAC_PACKET_BUF_SIZE];
        uint16_t blen = 0;
        bool gave_up = false;
        uint16_t gave_up_seq = 0;
        uint8_t gave_up_dest[AKIRA_MESH_NODE_ID_LEN];
        uint8_t gave_up_msg_type = 0;
        uint32_t elapsed_ms = 0;
        uint8_t retries = 0;
        uint16_t retransmit_seq = 0;

        k_mutex_lock(&s_transport.lock, K_FOREVER);
        struct pending_ack *e = &s_transport.acks.e[i];
        elapsed_ms = now_ms - e->sent_at_ms;
        retries = e->retries;
        retransmit_seq = e->seq_num;
        mesh_ack_action_t act = mesh_ack_tick(e, now_ms, mesh_mac_ack_timeout_ms());
        if (act == MESH_ACK_RETRANSMIT) {
            blen = e->len;
            if (blen > sizeof(buf)) {
                blen = sizeof(buf);
            }
            memcpy(buf, e->payload, blen);
        } else if (act == MESH_ACK_GIVE_UP) {
            memcpy(gave_up_dest, e->dest_id, AKIRA_MESH_NODE_ID_LEN);
            gave_up_seq = e->seq_num;
            gave_up_msg_type = ((const struct mesh_header *)e->payload)->msg_type;
            e->active = false;
            gave_up = true;
        }
        k_mutex_unlock(&s_transport.lock);

        if (act == MESH_ACK_RETRANSMIT) {
            LOG_INF("DATA seq=%u no ack after %ums (retry %u), retransmitting",
                   retransmit_seq, elapsed_ms, retries);
            mesh_mac_send(MESH_MAC_PRIO_LOW, buf, blen);
        } else if (gave_up) {
            LOG_WRN("DATA seq=%u gave up after %ums (retries=%u)",
                   gave_up_seq, elapsed_ms, retries);
            /* App/stream frames already fail the whole transfer on a real
             * give-up (their own wait logic) — no need to also nuke the
             * shared route over one of many frames in a transfer, which
             * forces a full rediscovery mid-transfer for what may just be
             * one slow ack. Plain DATA sends keep the existing behavior. */
            if (gave_up_msg_type != AKIRA_MESH_MSG_APP_START &&
                gave_up_msg_type != AKIRA_MESH_MSG_APP_CHUNK &&
                gave_up_msg_type != AKIRA_MESH_MSG_STREAM_DATA &&
                router) {
                router->notify_unreachable(gave_up_dest);
            }
            if (s_transport.ack_notify_cb) {
                s_transport.ack_notify_cb(gave_up_seq, gave_up_dest, true);
            }
        }
    }
}

uint32_t mesh_transport_next_ack_deadline_ms(void)
{
    uint32_t next = UINT32_MAX;
    k_mutex_lock(&s_transport.lock, K_FOREVER);
    for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_PENDING_ACKS; i++) {
        struct pending_ack *e = &s_transport.acks.e[i];
        if (e->active && (next == UINT32_MAX || (int32_t)(e->deadline_ms - next) < 0)) {
            next = e->deadline_ms;
        }
    }
    k_mutex_unlock(&s_transport.lock);
    return next;
}
