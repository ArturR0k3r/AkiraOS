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
#include "mesh_routing.h"
#include "lib/mem_helper.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/random/random.h>
#include <string.h>
#include "storage/fs_manager.h"
#include <stddef.h>
#if defined(CONFIG_AKIRA_MESH_APP_AUTO_INSTALL)
#include "runtime/app_manager/app_manager.h"
#endif

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
struct __packed aodv_rreq {
    uint8_t  target[AKIRA_MESH_NODE_ID_LEN];
    uint16_t rreq_id;
    uint16_t orig_seq;
    uint16_t dest_seq;
};
struct __packed aodv_rrep {
    uint8_t  target[AKIRA_MESH_NODE_ID_LEN];
    uint16_t dest_seq;
    uint8_t  hop_count;
};
struct __packed aodv_rerr {
    uint8_t  unreachable[AKIRA_MESH_NODE_ID_LEN];
    uint16_t dest_seq;
};

/* ---- WASM app distribution over mesh ---- */
#define MESH_APP_START_MAGIC 0x414B4D53u  /* 'A','K','M','S' */

/* One-shot header carrying the transfer invariants — sent once per transfer,
 * not repeated per chunk. */
struct __packed mesh_app_start_hdr {
    uint32_t magic;
    uint32_t app_id;                  /* crc32_ieee(app_name) — transfer id */
    uint32_t total_len;
    uint16_t chunk_count;
    char     app_name[AKIRA_MESH_APP_NAME_LEN];
    uint16_t crc;                     /* crc16_ccitt over all preceding fields */
};

/* Per-chunk header. chunk_len is NOT carried on the wire — the receiver
 * derives it from the received frame length, since the mesh RX path already
 * knows exactly how many bytes arrived. */
struct __packed mesh_app_chunk_hdr {
    uint32_t app_id;
    uint16_t chunk_index;             /* 0-based */
    uint16_t crc;                     /* crc16_ccitt over app_id+chunk_index then the body */
};

/* Bitmap sizing: a conservative minimum stride bounds the worst-case chunk
 * count for a transfer at the app-size ceiling, so the received-chunk bitmap
 * can be a fixed-size array instead of a runtime allocation. Real per-packet
 * stride is always far larger (~226B at the LR2021's 255B MTU), so actual
 * chunk_index values stay well under the wire field's uint16_t range even
 * at the largest configured app size. */
#define MESH_APP_MIN_STRIDE   16
#define MESH_APP_MAX_CHUNKS   ((CONFIG_AKIRA_APP_MAX_SIZE_KB * 1024) / MESH_APP_MIN_STRIDE)
#define MESH_APP_BITMAP_BYTES DIV_ROUND_UP(MESH_APP_MAX_CHUNKS, 8)

/* Mesh apps now stream straight to storage (no RAM reassembly buffer) —
 * this is where an in-flight transfer's chunks are appended to before the
 * final rename to its canonical install path. Only one transfer is ever
 * in flight (single-slot app_rx), so a fixed name is enough. */
#define MESH_APP_TMP_PATH_SD    "/SD:/apps/.mesh_recv.wasm"
#define MESH_APP_TMP_PATH_FLASH "/lfs/apps/.mesh_recv.wasm"
#define MESH_APP_TMP_PATH_MAX   40

/* Resume support: sender asks what the receiver already has for an app_id
 * before (re-)sending chunks, so a retried transfer only fills gaps instead
 * of resending everything from scratch. Best-effort (not ack-tracked) — a
 * lost/unanswered query just falls back to sending every chunk, same as
 * before this existed. */
struct __packed mesh_app_status_req {
    uint32_t app_id;
};
struct __packed mesh_app_status_resp {
    uint32_t app_id;
    uint16_t chunk_count;
    uint16_t received_count;
    /* received-bitmap bytes follow, ceil(chunk_count/8) of them — derived
     * from the received frame length, same convention as APP_CHUNK's body. */
};

/* Mesh manager state */
static struct {
    akira_mesh_config_t    config;
    akira_mesh_stats_t     stats;
    radio_handle_t        *radio;
    size_t                 mtu;         /* max single-frame TX payload, queried at init */
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
    struct k_mutex         tables_lock; /* guards tables + node table + counters */
    bool initialized;
    bool started;

    /* WASM app reassembly. RX-thread-only: mesh_dispatch is the sole reader
     * and writer, always on the RX thread, so this needs no lock. */
    struct {
        bool     active;
        bool     use_sd;              /* destination picked at APP_START */
        uint32_t app_id;
        uint32_t total_len;
        uint16_t chunk_count;
        uint16_t received_count;
        uint32_t last_activity_ms;   /* k_uptime_get_32() of last accepted START/chunk */
        uint8_t  origin_id[AKIRA_MESH_NODE_ID_LEN];
        char     app_name[AKIRA_MESH_APP_NAME_LEN];
        char     tmp_path[MESH_APP_TMP_PATH_MAX];
        uint8_t  received[MESH_APP_BITMAP_BYTES];
    } app_rx;
} mesh_state
#if defined(CONFIG_SPIRAM)
  __attribute__((section(".ext_ram.bss")))
#endif
;

/* Single-slot wait for an app-distribution frame's real E2E ack (see
 * mesh_send_app_and_wait). Guarded by tables_lock. Distribution sends one
 * frame at a time and blocks for its ack before the next, so one slot
 * covers the whole transfer, same scope as app_rx. */
static struct {
    bool     active;
    bool     acked;
    uint16_t seq;
    uint8_t  dest[AKIRA_MESH_NODE_ID_LEN];
    struct k_sem sem;
} s_app_ack_wait;

/* Single-slot wait for an APP_STATUS_RESP (see mesh_query_app_rx_progress).
 * Same single-in-flight-transfer scope as s_app_ack_wait/app_rx. bitmap_len
 * is however many bytes the response actually carried (may be less than
 * ceil(chunk_count/8) if it didn't fit in one frame) — bits beyond that are
 * treated as unknown, never as "already received". */
static struct {
    bool     active;
    bool     got_resp;
    uint32_t app_id;
    uint8_t  dest[AKIRA_MESH_NODE_ID_LEN];
    uint16_t received_count;
    uint8_t  bitmap[MESH_APP_BITMAP_BYTES];
    size_t   bitmap_len;
    struct k_sem sem;
} s_app_status_wait;

static K_MUTEX_DEFINE(mesh_init_lock);   /* serialize init/stop */

/* Forward declarations */
static void mesh_dispatch(const uint8_t *buf, size_t len);
static void update_neighbor(const uint8_t *src_id, const uint8_t *payload, size_t plen);
static void send_rerr(const uint8_t *to_immediate, const uint8_t *unreachable);
static void send_rreq(const uint8_t *target);
static int  mesh_send_reliable(const uint8_t *dest_id, uint8_t msg_type,
                               const uint8_t *data, size_t len);
static int  mesh_send_app_and_wait(const uint8_t *dest_id, uint8_t msg_type,
                                   const uint8_t *data, size_t len);
static int  mesh_query_app_rx_progress(const uint8_t *dest_id, uint32_t app_id,
                                       uint8_t *bitmap_out, size_t bitmap_cap,
                                       uint16_t *received_count_out);
static bool mesh_app_rx_start(const uint8_t *src_id, const struct mesh_app_start_hdr *s);
static bool mesh_app_rx_chunk(const struct mesh_app_chunk_hdr *ch,
                              const uint8_t *data, size_t data_len);

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
    /* tables_lock guards counters too (see state comment); beacon (workqueue
     * thread) and app-distribute (shell thread) call this concurrently now,
     * so the increment needs the same lock. Recursive-safe if a caller
     * already holds it. */
    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    h->seq_num = mesh_state.seq_num++;
    k_mutex_unlock(&mesh_state.tables_lock);
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
         * mesh_send_reliable re-takes tables_lock — so we must NOT hold it here. */
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

        /* The queued payload is a full packet (header + data); re-send it as
         * whatever message type it actually was — a queued APP_CHUNK must not
         * flush as DATA or the receiver's reassembler never sees it. */
        if (l < sizeof(struct mesh_header)) {
            continue;
        }
        const struct mesh_header *qh = (const struct mesh_header *)buf;
        mesh_send_reliable(dest, qh->msg_type, buf + sizeof(*qh), l - sizeof(*qh));
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
/* WASM app reassembly — RX-thread-only (see mesh_state.app_rx).       */
/* ------------------------------------------------------------------ */

/* Pick where a transfer of `total_len` bytes will land: SD if available and
 * it actually has room for the WHOLE app, else internal flash if it does,
 * else neither — the caller must reject the transfer rather than accept it
 * and run out of room mid-stream. Checked once, upfront, per transfer: once
 * bytes are appended, redirecting to another backend would need re-buffering
 * or a re-fetch protocol, so the whole app must fit the chosen backend. */
static bool mesh_app_pick_dest(uint32_t total_len, bool *use_sd_out, const char **path_out)
{
    fs_info_t info;

    if (fs_manager_sd_available() &&
        fs_manager_get_type_info(FS_TYPE_SD_CARD, &info) == 0 &&
        info.free_bytes >= total_len) {
        *use_sd_out = true;
        *path_out = MESH_APP_TMP_PATH_SD;
        return true;
    }
    if (fs_manager_get_type_info(FS_TYPE_INTERNAL, &info) == 0 &&
        info.free_bytes >= total_len) {
        *use_sd_out = false;
        *path_out = MESH_APP_TMP_PATH_FLASH;
        return true;
    }
    return false;
}

/* Returns true iff this START was genuinely accepted (including the
 * idempotent already-in-flight no-op). Callers must ack only on true — a
 * frame acked without acceptance makes the sender assume its content was
 * taken and proceed through the whole transfer while this end rejected it. */
static bool mesh_app_rx_start(const uint8_t *src_id, const struct mesh_app_start_hdr *s)
{
    if (s->magic != MESH_APP_START_MAGIC) {
        return false;
    }
    /* Validate CRC before trusting any field — a corrupted START (esp. a
     * retransmit with a garbled app_id) must be cleanly dropped, not treated
     * as a new transfer that would wipe an in-flight one. */
    if (crc16_ccitt(0xFFFF, (const uint8_t *)s,
                    offsetof(struct mesh_app_start_hdr, crc)) != s->crc) {
        LOG_WRN("Mesh APP_START dropped: CRC mismatch");
        return false;
    }
    if (s->chunk_count == 0 || s->chunk_count > MESH_APP_MAX_CHUNKS || s->total_len == 0 ||
        s->total_len > (uint32_t)CONFIG_AKIRA_APP_MAX_SIZE_KB * 1024) {
        LOG_WRN("Mesh APP_START rejected: len=%u chunks=%u exceeds size limits",
                s->total_len, s->chunk_count);
        return false;
    }

    uint32_t now = k_uptime_get_32();
    bool stale = (now - mesh_state.app_rx.last_activity_ms) > CONFIG_AKIRA_MESH_APP_RX_STALE_MS;

    /* The sender's own retransmit machinery can resend APP_START (e.g. the
     * real ack took >ACK_TIMEOUT_MS) even after we already started/finished
     * accepting chunks for it. A duplicate for the SAME still-live transfer
     * must be a no-op, not a reset, or it wipes received_count mid-transfer
     * with no way to recover the chunks already delivered. But if the sender
     * gave up and nothing arrived for a while, this is a genuine retry (same
     * app_id, since it's just crc32(name)) — let it restart, or a stalled
     * transfer would block that name forever. */
    if (mesh_state.app_rx.active && mesh_state.app_rx.app_id == s->app_id && !stale) {
        LOG_DBG("Mesh APP_START duplicate for in-flight app_id 0x%08x, ignoring (%u/%u chunks so far)",
                s->app_id, mesh_state.app_rx.received_count, mesh_state.app_rx.chunk_count);
        return true;
    }

    bool use_sd;
    const char *tmp_path;
    if (!mesh_app_pick_dest(s->total_len, &use_sd, &tmp_path)) {
        LOG_WRN("Mesh APP_START rejected: no storage with %u free bytes (SD or flash)",
                s->total_len);
        return false;
    }
    /* Reset any leftover temp file from a previous aborted transfer — the
     * append below must start from empty, not extend stale bytes. */
    fs_manager_delete_file(tmp_path);

    memset(&mesh_state.app_rx, 0, sizeof(mesh_state.app_rx));
    mesh_state.app_rx.active = true;
    mesh_state.app_rx.use_sd = use_sd;
    mesh_state.app_rx.app_id = s->app_id;
    mesh_state.app_rx.total_len = s->total_len;
    mesh_state.app_rx.chunk_count = s->chunk_count;
    mesh_state.app_rx.last_activity_ms = now;
    memcpy(mesh_state.app_rx.origin_id, src_id, AKIRA_MESH_NODE_ID_LEN);
    memcpy(mesh_state.app_rx.app_name, s->app_name, AKIRA_MESH_APP_NAME_LEN);
    mesh_state.app_rx.app_name[AKIRA_MESH_APP_NAME_LEN - 1] = '\0';
    strncpy(mesh_state.app_rx.tmp_path, tmp_path, MESH_APP_TMP_PATH_MAX - 1);
    mesh_state.app_rx.tmp_path[MESH_APP_TMP_PATH_MAX - 1] = '\0';

    LOG_INF("Mesh app transfer starting: '%s' (%u bytes, %u chunks, dest=%s)",
            mesh_state.app_rx.app_name, s->total_len, s->chunk_count,
            use_sd ? "SD" : "flash");
    return true;
}

/* Returns true iff this chunk was genuinely accepted (including a repeat of
 * the last one already written). Callers gate send_ack() on this, same as
 * mesh_app_rx_start. */
static bool mesh_app_rx_chunk(const struct mesh_app_chunk_hdr *ch,
                              const uint8_t *data, size_t data_len)
{
    if (!mesh_state.app_rx.active) {
        return false;
    }
    /* Validate header+body together before trusting any header field:
     * without covering app_id/chunk_index in the CRC, a corrupted header
     * byte is indistinguishable from a genuine mismatch or duplicate and
     * gets silently misrouted instead of cleanly dropped. */
    uint16_t hdr_crc = crc16_ccitt(0xFFFF, (const uint8_t *)ch,
                                   offsetof(struct mesh_app_chunk_hdr, crc));
    if (crc16_ccitt(hdr_crc, data, data_len) != ch->crc) {
        LOG_WRN("Mesh app chunk dropped: CRC mismatch (header or body)");
        return false;
    }
    if (ch->app_id != mesh_state.app_rx.app_id) {
        LOG_WRN("Mesh app chunk %u dropped: app_id mismatch", ch->chunk_index);
        return false;
    }
    if (ch->chunk_index >= mesh_state.app_rx.chunk_count) {
        LOG_WRN("Mesh app chunk %u dropped: index >= chunk_count", ch->chunk_index);
        return false;
    }

    /* Stride matches the sender's exactly: both ends are the same firmware
     * on the same pinned radio, so they derive it from the same MTU. */
    size_t stride = mesh_state.mtu - sizeof(struct mesh_header) -
                    sizeof(struct mesh_app_chunk_hdr);
    size_t off = (size_t)ch->chunk_index * stride;
    if (off + data_len > mesh_state.app_rx.total_len) {
        LOG_WRN("Mesh app chunk %u dropped: exceeds total_len", ch->chunk_index);
        return false;
    }

    /* Chunks are sent strictly in order — the sender blocks on a real
     * end-to-end ack for chunk N before ever sending N+1 — so a receiver
     * only ever sees the next expected index, or a retransmit-duplicate of
     * the one it just wrote (now correctly re-acked without reprocessing,
     * see mesh_dispatch). No out-of-order arrival is possible, so straight
     * append — no positioned writes, no RAM reassembly buffer — is safe. */
    if (ch->chunk_index != mesh_state.app_rx.received_count) {
        if (mesh_state.app_rx.received_count > 0 &&
            ch->chunk_index == mesh_state.app_rx.received_count - 1) {
            mesh_state.app_rx.last_activity_ms = k_uptime_get_32();
            return true;
        }
        LOG_WRN("Mesh app chunk %u dropped: out of order, expected %u",
                ch->chunk_index, mesh_state.app_rx.received_count);
        return false;
    }

    /* First chunk truncates (write), rest append. The truncating write
     * guarantees a clean start even if the pre-START delete of a leftover
     * temp file from a prior aborted transfer silently failed — otherwise
     * an unchecked stale file would get extended, corrupting the result. */
    ssize_t written = (mesh_state.app_rx.received_count == 0)
        ? fs_manager_write_file(mesh_state.app_rx.tmp_path, data, data_len)
        : fs_manager_append_file(mesh_state.app_rx.tmp_path, data, data_len);
    if (written < 0 || (size_t)written != data_len) {
        LOG_ERR("Mesh app chunk %u write failed: %zd", ch->chunk_index, written);
        return false;
    }
    mesh_state.app_rx.last_activity_ms = k_uptime_get_32();

    size_t bit = ch->chunk_index;
    mesh_state.app_rx.received[bit / 8] |= BIT(bit % 8);
    mesh_state.app_rx.received_count++;

    if (mesh_state.app_rx.received_count < mesh_state.app_rx.chunk_count) {
        return true;
    }

    /* Reassembly complete. */
    mesh_state.app_rx.active = false;
    LOG_INF("Mesh app '%s' reassembled (%u bytes, %u chunks)",
            mesh_state.app_rx.app_name, mesh_state.app_rx.total_len,
            mesh_state.app_rx.chunk_count);

    if (mesh_state.rx_callback) {
        /* Opt-in read-back: only when something is actually listening, so
         * the mainline path (auto-install, no callback) stays fully
         * streaming with zero extra RAM use. */
        uint8_t *buf = akira_malloc_buffer(mesh_state.app_rx.total_len);
        if (buf) {
            ssize_t n = fs_manager_read_file(mesh_state.app_rx.tmp_path, buf,
                                             mesh_state.app_rx.total_len);
            if (n == (ssize_t)mesh_state.app_rx.total_len) {
                mesh_state.rx_callback(mesh_state.app_rx.origin_id, buf,
                                       mesh_state.app_rx.total_len, mesh_state.rx_user_data);
            }
            akira_free_buffer(buf);
        } else {
            LOG_WRN("Mesh app '%s' rx_callback skipped: read-back alloc failed",
                    mesh_state.app_rx.app_name);
        }
    }

#if defined(CONFIG_AKIRA_MESH_APP_AUTO_INSTALL)
    int id = app_manager_register_installed(mesh_state.app_rx.app_name,
                                            mesh_state.app_rx.tmp_path,
                                            mesh_state.app_rx.total_len, APP_SOURCE_MESH,
                                            mesh_state.app_rx.use_sd);
    if (id < 0) {
        LOG_ERR("Mesh app '%s' auto-install failed: %d", mesh_state.app_rx.app_name, id);
    }
#else
    fs_manager_delete_file(mesh_state.app_rx.tmp_path);
#endif
    return true;
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

    /* Duplicate suppression. Skipped for:
     *  - unicast ACK (echoes an acked seq, never deduped)
     *  - APP_START/APP_CHUNK addressed to US: the app layer re-validates
     *    every arrival (CRC/app_id/order) and handles duplicates itself, and
     *    unlike plain DATA these can REJECT a first arrival — the seen-cache
     *    only records that a seq_num arrived, not whether it was accepted, so
     *    a "seen" bit here would permanently swallow a clean retry behind a
     *    corrupted first attempt.
     * App frames we FORWARD (dest != us) are still deduped: a relay doesn't
     * validate content, so seen-based loop/flood suppression is both safe and
     * wanted there. */
    bool is_app_frame = (h->msg_type == AKIRA_MESH_MSG_APP_START ||
                         h->msg_type == AKIRA_MESH_MSG_APP_CHUNK);
    bool skip_dedup = (h->msg_type == AKIRA_MESH_MSG_ACK) ||
                      (is_app_frame && is_self(h->dest_id));
    if (!skip_dedup) {
        k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
        bool dup = mesh_seen_check_and_add(&mesh_state.seen, h->src_id, h->seq_num);
        k_mutex_unlock(&mesh_state.tables_lock);
        if (dup) {
            /* We already processed this frame — the sender's retry means our
             * ack for it was lost, not that the payload needs reprocessing.
             * Re-ack without reprocessing so the sender's retry loop can
             * complete instead of exhausting retries against a silent drop.
             * Safe here: DATA has no rejection path, so "seen" does imply
             * "accepted" for this message type specifically. */
            if (is_self(h->dest_id) && h->msg_type == AKIRA_MESH_MSG_DATA) {
                send_ack(h->src_id, h->seq_num);
            }
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
        if (is_self(h->src_id)) {
            /* Our own RREQ, rebroadcast back to us by a neighbor. Forwarding
             * it again loops it around the mesh indefinitely (each hop still
             * sees a "fresh" src+seq the first time it arrives there). */
            break;
        }
        const struct aodv_rreq *rq = (const struct aodv_rreq *)payload;
        k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
        mesh_route_install(&mesh_state.routes, h->src_id, h->src_id, 1,
                           rq->orig_seq, now + MESH_ROUTE_LIFETIME_MS);
        k_mutex_unlock(&mesh_state.tables_lock);
        if (is_self(rq->target)) {
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
            send_ack(h->src_id, h->seq_num);
            if (mesh_state.rx_callback) {
                mesh_state.rx_callback(h->src_id, payload, plen,
                                       mesh_state.rx_user_data);
            }
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
        if (s_app_ack_wait.active && h->seq_num == s_app_ack_wait.seq &&
            memcmp(h->src_id, s_app_ack_wait.dest, AKIRA_MESH_NODE_ID_LEN) == 0) {
            s_app_ack_wait.active = false;
            s_app_ack_wait.acked = true;
            k_sem_give(&s_app_ack_wait.sem);
        }
        k_mutex_unlock(&mesh_state.tables_lock);
        break;

    /* App distribution is unicast-only (see akira_mesh_distribute_app) — no
     * broadcast branch, just deliver-or-forward like the direct DATA case. */
    case AKIRA_MESH_MSG_APP_START:
        if (is_self(h->dest_id)) {
            /* Ack only on genuine acceptance: an unconditional ack makes the
             * sender assume the content was taken and proceed through the
             * whole transfer while this end silently rejected it. */
            if (plen >= sizeof(struct mesh_app_start_hdr) &&
                mesh_app_rx_start(h->src_id, (const struct mesh_app_start_hdr *)payload)) {
                send_ack(h->src_id, h->seq_num);
            }
        } else {
            forward_data((struct mesh_header *)buf, buf, len);
        }
        break;

    case AKIRA_MESH_MSG_APP_CHUNK:
        if (is_self(h->dest_id)) {
            if (plen >= sizeof(struct mesh_app_chunk_hdr)) {
                const struct mesh_app_chunk_hdr *ch = (const struct mesh_app_chunk_hdr *)payload;
                if (mesh_app_rx_chunk(ch, payload + sizeof(*ch), plen - sizeof(*ch))) {
                    send_ack(h->src_id, h->seq_num);
                }
            }
        } else {
            forward_data((struct mesh_header *)buf, buf, len);
        }
        break;

    /* Resume support (see mesh_query_app_rx_progress) — best-effort, not
     * ack-tracked: a lost query/response just means the sender falls back
     * to sending every chunk, same as if this didn't exist. */
    case AKIRA_MESH_MSG_APP_STATUS_REQ:
        if (is_self(h->dest_id)) {
            if (plen < sizeof(struct mesh_app_status_req)) {
                break;
            }
            const struct mesh_app_status_req *req =
                (const struct mesh_app_status_req *)payload;

            uint8_t resp_pkt[MESH_PACKET_BUF_SIZE];
            struct mesh_header *rh = (struct mesh_header *)resp_pkt;
            fill_header(rh, AKIRA_MESH_MSG_APP_STATUS_RESP, 1, h->src_id);
            struct mesh_app_status_resp *resp =
                (struct mesh_app_status_resp *)(resp_pkt + sizeof(*rh));
            size_t max_bitmap = sizeof(resp_pkt) - sizeof(*rh) - sizeof(*resp);
            size_t bitmap_len = 0;

            resp->app_id = req->app_id;
            if (mesh_state.app_rx.active && mesh_state.app_rx.app_id == req->app_id) {
                resp->chunk_count = mesh_state.app_rx.chunk_count;
                resp->received_count = mesh_state.app_rx.received_count;
                bitmap_len = MIN(DIV_ROUND_UP(mesh_state.app_rx.chunk_count, 8), max_bitmap);
                memcpy((uint8_t *)resp + sizeof(*resp), mesh_state.app_rx.received, bitmap_len);
            } else {
                resp->chunk_count = 0;
                resp->received_count = 0;
            }
            mesh_radio_tx(resp_pkt, sizeof(*rh) + sizeof(*resp) + bitmap_len);
        } else {
            forward_data((struct mesh_header *)buf, buf, len);
        }
        break;

    case AKIRA_MESH_MSG_APP_STATUS_RESP:
        if (is_self(h->dest_id)) {
            if (plen < sizeof(struct mesh_app_status_resp)) {
                break;
            }
            const struct mesh_app_status_resp *resp =
                (const struct mesh_app_status_resp *)payload;
            k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
            if (s_app_status_wait.active && resp->app_id == s_app_status_wait.app_id &&
                memcmp(h->src_id, s_app_status_wait.dest, AKIRA_MESH_NODE_ID_LEN) == 0) {
                size_t bitmap_len = plen - sizeof(*resp);
                if (bitmap_len > sizeof(s_app_status_wait.bitmap)) {
                    bitmap_len = sizeof(s_app_status_wait.bitmap);
                }
                s_app_status_wait.received_count = resp->received_count;
                memcpy(s_app_status_wait.bitmap, payload + sizeof(*resp), bitmap_len);
                s_app_status_wait.bitmap_len = bitmap_len;
                s_app_status_wait.got_resp = true;
                s_app_status_wait.active = false;
                k_sem_give(&s_app_status_wait.sem);
            }
            k_mutex_unlock(&mesh_state.tables_lock);
        } else {
            forward_data((struct mesh_header *)buf, buf, len);
        }
        break;

    default:
        LOG_DBG("Unhandled mesh message type: %d", h->msg_type);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* RX thread — polls the radio under the bus lock, dispatches without. */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_SPIRAM)
static uint8_t s_rx_buf[MESH_PACKET_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
#else
static uint8_t s_rx_buf[MESH_PACKET_BUF_SIZE];
#endif

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
        } else {
            /* The IRQ-driven recv path returns almost instantly regardless
             * of whether a packet arrived (two quick status reads, no real
             * blocking) — with no sleep here this becomes a tight loop that
             * re-acquires the shared SPI bus lock continuously, starving
             * other bus users (e.g. the SD card, which shares this SPI
             * controller) of a fair turn to even start their own sequence. */
            k_msleep(CONFIG_AKIRA_MESH_RX_IDLE_YIELD_MS);
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
            uint16_t gave_up_seq = e->seq_num;
            uint8_t gave_up_msg_type = ((const struct mesh_header *)e->payload)->msg_type;
            e->active = false;
            /* App frames already fail the whole transfer on a real give-up
             * (mesh_send_app_and_wait) — no need to also nuke the shared
             * route over one of the 18+ frames in a transfer, which forces
             * a full AODV rediscovery mid-transfer for what may just be one
             * slow ack. Plain DATA sends keep the existing behavior. */
            if (gave_up_msg_type != AKIRA_MESH_MSG_APP_START &&
                gave_up_msg_type != AKIRA_MESH_MSG_APP_CHUNK) {
                mesh_route_invalidate(&mesh_state.routes, dest);
            }
            if (s_app_ack_wait.active && gave_up_seq == s_app_ack_wait.seq &&
                memcmp(dest, s_app_ack_wait.dest, AKIRA_MESH_NODE_ID_LEN) == 0) {
                s_app_ack_wait.active = false;
                k_sem_give(&s_app_ack_wait.sem);   /* acked stays false -> timeout path */
            }
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
    s_app_ack_wait.active = false;
    k_sem_init(&s_app_ack_wait.sem, 0, 1);
    s_app_status_wait.active = false;
    k_sem_init(&s_app_status_wait.sem, 0, 1);

    mesh_state.radio = radio_manager_acquire_by_caps(config->transport_caps, "mesh");
    if (!mesh_state.radio) {
        LOG_ERR("No radio matches mesh transport caps 0x%08x", config->transport_caps);
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
            k_mutex_unlock(&mesh_init_lock);
            return rret;
        }
    }

    /* MTU for this radio's current config. Radios without get_max_payload()
     * keep the historical MESH_PACKET_BUF_SIZE bound (unchanged behavior);
     * app-chunk distribution separately refuses to run without a real query,
     * since its chunk stride must match what the radio will actually accept. */
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
        if (mesh_state.radio->ops && mesh_state.radio->ops->deinit) {
            mesh_state.radio->ops->deinit(mesh_state.radio);
        }
        radio_manager_release(mesh_state.radio, "mesh");
        mesh_state.radio = NULL;
    }
    mesh_state.initialized = false;
    k_mutex_unlock(&mesh_init_lock);
    LOG_INF("AkiraMesh stopped");
    return 0;
}

/* Sends one app-distribution frame (START or CHUNK) and blocks for its real
 * end-to-end ack — not just an ack-table slot. mesh_send_reliable (below)
 * only waits for a free slot, so a chunk that never actually gets delivered
 * (retries exhaust, GIVE_UP) is silently dropped while the caller still sees
 * "success" — the whole transfer then reports as distributed while the
 * receiver's reassembly can never complete. Blocking per-frame here makes
 * that failure visible and stops the sender from ever getting ahead of a
 * frame that didn't make it, so START-before-chunks ordering holds too.
 *
 * A beacon-discovered neighbor has no AODV route yet (routes are only
 * installed by RREQ/RREP, beacons only populate the node list) — so a fresh
 * target needs one RREQ/RREP round trip here before the real send. */
static int mesh_send_app_and_wait(const uint8_t *dest_id, uint8_t msg_type,
                                  const uint8_t *data, size_t len)
{
    uint32_t now = k_uptime_get_32();
    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    struct route_entry *r = mesh_route_lookup(&mesh_state.routes, dest_id, now);
    k_mutex_unlock(&mesh_state.tables_lock);

    if (!r) {
        send_rreq(dest_id);
        uint32_t waited_ms = 0;
        const uint32_t route_wait_budget_ms =
            CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS * (CONFIG_AKIRA_MESH_MAX_RETRIES + 1);
        /* RREQ is a fire-and-forget broadcast — no ack/retry like unicast
         * frames get. A single lost RREQ (or its RREP) would otherwise burn
         * the whole wait budget doing nothing and return -EHOSTUNREACH with
         * no recovery. Resend periodically instead of once (standard AODV). */
        uint32_t since_last_rreq_ms = 0;
        while (!r && waited_ms < route_wait_budget_ms) {
            k_sleep(K_MSEC(CONFIG_AKIRA_MESH_APP_TX_GAP_MS));
            waited_ms += CONFIG_AKIRA_MESH_APP_TX_GAP_MS;
            since_last_rreq_ms += CONFIG_AKIRA_MESH_APP_TX_GAP_MS;
            if (since_last_rreq_ms >= CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS) {
                send_rreq(dest_id);
                since_last_rreq_ms = 0;
            }
            k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
            r = mesh_route_lookup(&mesh_state.routes, dest_id, k_uptime_get_32());
            k_mutex_unlock(&mesh_state.tables_lock);
        }
        if (!r) {
            return -EHOSTUNREACH;
        }
    }

    now = k_uptime_get_32();
    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    uint8_t pkt[MESH_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_header(h, msg_type, mesh_state.config.max_hops, dest_id);
    memcpy(pkt + sizeof(*h), data, len);
    size_t total = sizeof(*h) + len;
    uint16_t seq = h->seq_num;

    int slot = mesh_ack_add(&mesh_state.acks, seq, dest_id, pkt, total,
                            now + CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS);
    if (slot < 0) {
        k_mutex_unlock(&mesh_state.tables_lock);
        return -EBUSY;
    }

    /* Arm the wait before transmitting: the receiver cannot generate an ack
     * for a packet not yet sent, so registering here (same lock section as
     * ack_add, before mesh_radio_tx) closes the race against the RX thread. */
    s_app_ack_wait.seq = seq;
    memcpy(s_app_ack_wait.dest, dest_id, AKIRA_MESH_NODE_ID_LEN);
    s_app_ack_wait.acked = false;
    s_app_ack_wait.active = true;
    k_sem_reset(&s_app_ack_wait.sem);
    k_mutex_unlock(&mesh_state.tables_lock);

    mesh_state.stats.messages_sent++;
    mesh_radio_tx(pkt, total);

    int wait_ret = k_sem_take(&s_app_ack_wait.sem,
                              K_MSEC(CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS *
                                     (CONFIG_AKIRA_MESH_MAX_RETRIES + 2)));

    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    bool acked = s_app_ack_wait.acked;
    s_app_ack_wait.active = false;
    if (!acked) {
        /* Stop the generic retransmit machinery from continuing to resend
         * this frame in the background after we've already given up on it —
         * otherwise a zombie retransmit can land long after the caller has
         * moved on (or aborted the whole transfer), confusing the receiver
         * into starting/continuing a transfer the sender no longer drives. */
        mesh_ack_clear(&mesh_state.acks, seq, dest_id);
    }
    k_mutex_unlock(&mesh_state.tables_lock);

    return (wait_ret == 0 && acked) ? 0 : -ETIMEDOUT;
}

/* Best-effort query of the receiver's current reassembly progress for
 * app_id, so a retried distribute_app can skip chunks it already has.
 * Not ack-tracked/retried — on any failure (no route, no reply in time)
 * returns non-zero and the caller should just fall back to sending every
 * chunk, exactly as if this query didn't exist. */
static int mesh_query_app_rx_progress(const uint8_t *dest_id, uint32_t app_id,
                                      uint8_t *bitmap_out, size_t bitmap_cap,
                                      uint16_t *received_count_out)
{
    uint32_t now = k_uptime_get_32();
    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    struct route_entry *r = mesh_route_lookup(&mesh_state.routes, dest_id, now);
    if (!r) {
        k_mutex_unlock(&mesh_state.tables_lock);
        return -EHOSTUNREACH;
    }

    uint8_t pkt[MESH_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_header(h, AKIRA_MESH_MSG_APP_STATUS_REQ, mesh_state.config.max_hops, dest_id);
    struct mesh_app_status_req *req = (struct mesh_app_status_req *)(pkt + sizeof(*h));
    req->app_id = app_id;
    size_t total = sizeof(*h) + sizeof(*req);

    /* Arm before sending — same race-closing reasoning as mesh_send_app_and_wait. */
    s_app_status_wait.app_id = app_id;
    memcpy(s_app_status_wait.dest, dest_id, AKIRA_MESH_NODE_ID_LEN);
    s_app_status_wait.got_resp = false;
    s_app_status_wait.active = true;
    k_sem_reset(&s_app_status_wait.sem);
    k_mutex_unlock(&mesh_state.tables_lock);

    mesh_radio_tx(pkt, total);

    int wait_ret = k_sem_take(&s_app_status_wait.sem, K_MSEC(CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS));

    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    s_app_status_wait.active = false;
    bool got = s_app_status_wait.got_resp;
    if (got) {
        *received_count_out = s_app_status_wait.received_count;
        size_t n = MIN(s_app_status_wait.bitmap_len, bitmap_cap);
        memcpy(bitmap_out, s_app_status_wait.bitmap, n);
        if (n < bitmap_cap) {
            memset(bitmap_out + n, 0, bitmap_cap - n);
        }
    }
    k_mutex_unlock(&mesh_state.tables_lock);

    return (wait_ret == 0 && got) ? 0 : -ETIMEDOUT;
}

/* Shared reliable-unicast path: build a packet of the given msg_type, send it
 * if a route exists (tracked for end-to-end ACK/retry), else queue it and
 * originate an RREQ. Used by plain DATA sends; a packet re-sent after route
 * discovery keeps its real message type (see flush_pending_for). */
static int mesh_send_reliable(const uint8_t *dest_id, uint8_t msg_type,
                              const uint8_t *data, size_t len)
{
    uint32_t now = k_uptime_get_32();
    bool have_route;
    k_mutex_lock(&mesh_state.tables_lock, K_FOREVER);
    struct route_entry *r = mesh_route_lookup(&mesh_state.routes, dest_id, now);
    have_route = (r != NULL);
    k_mutex_unlock(&mesh_state.tables_lock);

    uint8_t pkt[MESH_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_header(h, msg_type, mesh_state.config.max_hops, dest_id);
    memcpy(pkt + sizeof(*h), data, len);
    size_t total = sizeof(*h) + len;
    uint16_t seq = h->seq_num;

    if (have_route) {
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

    /* No route: queue payload + originate RREQ. */
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

int akira_mesh_send(const uint8_t *dest_id, const uint8_t *data, size_t len)
{
    if (!mesh_state.initialized || !dest_id || !data) {
        return -EINVAL;
    }
    if (!mesh_state.started) {
        return -ENODEV;
    }
    if (len > mesh_state.mtu - sizeof(struct mesh_header)) {
        return -EMSGSIZE;
    }

    if (is_broadcast(dest_id)) {
        return akira_mesh_broadcast(data, len, mesh_state.config.max_hops);
    }

    return mesh_send_reliable(dest_id, AKIRA_MESH_MSG_DATA, data, len);
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

/* Best-effort diagnostic snapshot: app_rx is RX-thread-owned with no lock
 * (see its declaration comment). Reading it from another thread (shell) can
 * race and show a torn/stale value — acceptable for a read-only debug
 * counter, not worth adding tables_lock to the RX hot path for. */
int akira_mesh_get_app_rx_status(akira_mesh_app_rx_status_t *out)
{
    if (!out) {
        return -EINVAL;
    }
    if (!mesh_state.initialized) {
        return -ENODEV;
    }
    out->active = mesh_state.app_rx.active;
    out->app_id = mesh_state.app_rx.app_id;
    memcpy(out->app_name, mesh_state.app_rx.app_name, AKIRA_MESH_APP_NAME_LEN);
    out->total_len = mesh_state.app_rx.total_len;
    out->chunk_count = mesh_state.app_rx.chunk_count;
    out->received_count = mesh_state.app_rx.received_count;
    out->age_ms = k_uptime_get_32() - mesh_state.app_rx.last_activity_ms;
    return 0;
}

int akira_mesh_register_rx_callback(akira_mesh_rx_cb_t callback, void *user_data)
{
    mesh_state.rx_callback = callback;
    mesh_state.rx_user_data = user_data;
    LOG_DBG("Mesh RX callback registered");
    return 0;
}

int akira_mesh_distribute_app(const uint8_t *dest_id, const char *app_name,
                              const uint8_t *app_data, size_t app_len)
{
    if (!dest_id || !app_name || !app_data || app_len == 0) {
        return -EINVAL;
    }
    if (!mesh_state.initialized || !mesh_state.started) {
        return -ENODEV;
    }
    /* Chunk stride is derived from the queried MTU, not the legacy
     * MESH_PACKET_BUF_SIZE fallback — a radio without get_max_payload() has
     * no verified TX limit to size chunks against. */
    if (!mesh_state.radio->ops || !mesh_state.radio->ops->get_max_payload) {
        LOG_ERR("Radio '%s' has no MTU query — app distribution unavailable",
                mesh_state.radio->name);
        return -ENOSYS;
    }
    size_t name_len = strlen(app_name);
    if (name_len == 0 || name_len >= AKIRA_MESH_APP_NAME_LEN) {
        return -ENAMETOOLONG;
    }

    size_t hdr_overhead = sizeof(struct mesh_header) + sizeof(struct mesh_app_chunk_hdr);
    if (mesh_state.mtu <= hdr_overhead) {
        return -EMSGSIZE;
    }
    size_t stride = mesh_state.mtu - hdr_overhead;
    size_t chunk_count_sz = (app_len + stride - 1) / stride;
    if (chunk_count_sz > MESH_APP_MAX_CHUNKS) {
        return -EMSGSIZE;
    }
    uint16_t chunk_count = (uint16_t)chunk_count_sz;
    uint32_t app_id = crc32_ieee((const uint8_t *)app_name, name_len);

    LOG_INF("Distributing WASM app '%s' (%zu bytes, %u chunks) over mesh",
            app_name, app_len, chunk_count);

    struct mesh_app_start_hdr start = {
        .magic = MESH_APP_START_MAGIC,
        .app_id = app_id,
        .total_len = (uint32_t)app_len,
        .chunk_count = chunk_count,
    };
    memcpy(start.app_name, app_name, name_len);
    /* CRC over every field but crc itself (the struct is zero-initialized, so
     * app_name padding past name_len is deterministic zeros on both ends).
     * Protects the START params — total_len/chunk_count/app_id/app_name — the
     * same way the chunk header is protected; without it a corrupted START
     * retransmit could silently reset an in-flight transfer. */
    start.crc = crc16_ccitt(0xFFFF, (const uint8_t *)&start,
                            offsetof(struct mesh_app_start_hdr, crc));

    /* Every frame (START and each chunk) blocks for its real end-to-end ack
     * before the next is sent — a frame that never actually gets delivered
     * must fail the whole transfer, not get silently dropped while the
     * caller still reports success. This also keeps chunks from ever
     * physically arriving before START. */
    int ret;
    int retries = 0;
    while ((ret = mesh_send_app_and_wait(dest_id, AKIRA_MESH_MSG_APP_START,
                                         (const uint8_t *)&start, sizeof(start))) == -EBUSY) {
        if (++retries > CONFIG_AKIRA_MESH_APP_TX_RETRIES) {
            return -EBUSY;
        }
        k_sleep(K_MSEC(CONFIG_AKIRA_MESH_APP_TX_GAP_MS));
    }
    if (ret < 0) {
        return ret;
    }

    /* Ask what the receiver already has (e.g. a prior attempt that got
     * partway before failing) so a retry only fills gaps instead of
     * resending everything. Best-effort — if it fails, resume_bitmap stays
     * all-zero and every chunk is sent. */
    /* Static, not stack: MESH_APP_BITMAP_BYTES scales with
     * CONFIG_AKIRA_APP_MAX_SIZE_KB and is too large for a shell thread's
     * stack frame. Single-in-flight-transfer scope, same as
     * s_app_ack_wait/s_app_status_wait/app_rx. */
    static uint8_t resume_bitmap[MESH_APP_BITMAP_BYTES];
    memset(resume_bitmap, 0, sizeof(resume_bitmap));
    uint16_t resume_received_count = 0;
    if (mesh_query_app_rx_progress(dest_id, app_id, resume_bitmap, sizeof(resume_bitmap),
                                   &resume_received_count) == 0 && resume_received_count > 0) {
        LOG_INF("Resuming '%s': receiver already has %u/%u chunks",
                app_name, resume_received_count, chunk_count);
    }

    uint8_t chunk_pkt[MESH_PACKET_BUF_SIZE - sizeof(struct mesh_header)];
    for (uint16_t i = 0; i < chunk_count; i++) {
        if (resume_bitmap[i / 8] & BIT(i % 8)) {
            continue;   /* receiver already reported having this one */
        }
        size_t off = (size_t)i * stride;
        size_t clen = MIN(stride, app_len - off);

        struct mesh_app_chunk_hdr *ch = (struct mesh_app_chunk_hdr *)chunk_pkt;
        ch->app_id = app_id;
        ch->chunk_index = i;
        /* Covers app_id+chunk_index too, not just the body — a header-only
         * bit error is otherwise undetectable and gets silently misrouted
         * instead of failing the CRC and being dropped. */
        uint16_t hdr_crc = crc16_ccitt(0xFFFF, (const uint8_t *)ch,
                                       offsetof(struct mesh_app_chunk_hdr, crc));
        ch->crc = crc16_ccitt(hdr_crc, app_data + off, clen);
        memcpy(chunk_pkt + sizeof(*ch), app_data + off, clen);

        retries = 0;
        while ((ret = mesh_send_app_and_wait(dest_id, AKIRA_MESH_MSG_APP_CHUNK,
                                             chunk_pkt, sizeof(*ch) + clen)) == -EBUSY) {
            if (++retries > CONFIG_AKIRA_MESH_APP_TX_RETRIES) {
                return -EBUSY;
            }
            k_sleep(K_MSEC(CONFIG_AKIRA_MESH_APP_TX_GAP_MS));
        }
        if (ret < 0) {
            return ret;
        }
    }

    mesh_state.stats.apps_distributed++;
    return 0;
}
