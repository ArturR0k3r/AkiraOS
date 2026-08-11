/**
 * @file mesh_app_dist.c
 * @brief AkiraMesh Application layer — WASM app distribution over mesh.
 *
 * Built entirely on mesh_transport_send_reliable()/mesh_router_get_active()
 * — no knowledge of AODV internals, MAC internals, or the ack_table's
 * storage. Wire protocol/reassembly logic here is unchanged from before the
 * layered split; only the calls it makes to reach the network changed.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include "mesh_app_dist.h"
#include "mesh_transport.h"
#include "mesh_router.h"
#include "mesh_mac.h"
#include "mesh_routing.h"
#include "lib/mem_helper.h"
#include "storage/fs_manager.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#if defined(CONFIG_AKIRA_MESH_APP_AUTO_INSTALL)
#include "runtime/app_manager/app_manager.h"
#endif

LOG_MODULE_REGISTER(akira_mesh_app_dist, CONFIG_AKIRA_LOG_LEVEL);

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

/* Mesh apps stream straight to storage (no RAM reassembly buffer) — this
 * is where an in-flight transfer's chunks are appended to before the final
 * rename to its canonical install path. Only one transfer is ever in
 * flight (single-slot app_rx), so a fixed name is enough. */
#define MESH_APP_TMP_PATH_SD    "/SD:/apps/.mesh_recv.wasm"
#define MESH_APP_TMP_PATH_FLASH "/lfs/apps/.mesh_recv.wasm"
#define MESH_APP_TMP_PATH_MAX   40

/* Resume support: sender asks what the receiver already has for an app_id
 * before (re-)sending chunks, so a retried transfer only fills gaps instead
 * of resending everything from scratch. Best-effort (not ack-tracked) — a
 * lost/unanswered query just falls back to sending every chunk. */
struct __packed mesh_app_status_req {
    uint32_t app_id;
};
struct __packed mesh_app_status_resp {
    uint32_t app_id;
    uint16_t chunk_count;
    uint16_t received_count;
    /* received-bitmap bytes follow, ceil(chunk_count/8) of them */
};

static struct {
    akira_mesh_config_t  config;
    akira_mesh_stats_t   *stats;
    akira_mesh_rx_cb_t   *rx_cb_ptr;
    void                **rx_ctx_ptr;
    uint16_t              seq_num;
    /* Only guards forwarded (non-self-destined) APP_* frames — a relay
     * doesn't validate content, so seen-based loop/flood suppression is
     * both safe and needed there, same reasoning as every other layer's
     * dedup gate. Frames addressed to us skip this deliberately: their own
     * chunk-index/CRC/app_id checks are idempotent and must still respond
     * (re-ack) to an exact retry, which a "seen" bit would permanently
     * swallow before it got the chance to. */
    struct seen_cache     seen;

    /* WASM app reassembly. RX-thread-only: mesh_app_dist_handle_frame is the
     * sole reader and writer, always on the RX thread, so this needs no lock. */
    struct {
        bool     active;
        bool     use_sd;
        uint32_t app_id;
        uint32_t total_len;
        uint16_t chunk_count;
        uint16_t received_count;
        uint32_t last_activity_ms;
        uint8_t  origin_id[AKIRA_MESH_NODE_ID_LEN];
        char     app_name[AKIRA_MESH_APP_NAME_LEN];
        char     tmp_path[MESH_APP_TMP_PATH_MAX];
        uint8_t  received[MESH_APP_BITMAP_BYTES];
    } app_rx;
} s_app_dist;

/* Single-slot wait for an app-distribution frame's real E2E ack (see
 * mesh_send_app_and_wait). Guarded implicitly by single-in-flight-transfer
 * usage, same scope as app_rx. */
static struct {
    bool     active;
    bool     acked;
    uint16_t seq;
    uint8_t  dest[AKIRA_MESH_NODE_ID_LEN];
    struct k_sem sem;
} s_app_ack_wait;

/* Single-slot wait for an APP_STATUS_RESP. bitmap_len is however many bytes
 * the response actually carried. */
static struct {
    bool     active;
    bool     got_resp;
    uint32_t app_id;
    uint8_t  dest[AKIRA_MESH_NODE_ID_LEN];
    uint16_t received_count;
    uint8_t  bitmap[MESH_APP_BITMAP_BYTES];
    size_t   bitmap_len;
    struct k_sem sem;
} s_app_status_wait AKIRA_BULK_BSS;

static bool is_self(const uint8_t *id)
{
    return memcmp(id, s_app_dist.config.node_id, AKIRA_MESH_NODE_ID_LEN) == 0;
}

static void fill_app_header(struct mesh_header *h, uint8_t type, uint8_t ttl, const uint8_t *dest)
{
    h->version = 1;
    h->msg_type = type;
    h->ttl = ttl;
    memcpy(h->src_id, s_app_dist.config.node_id, AKIRA_MESH_NODE_ID_LEN);
    memcpy(h->dest_id, dest, AKIRA_MESH_NODE_ID_LEN);
    h->seq_num = s_app_dist.seq_num++;
}

static void app_dist_on_ack_notify(uint16_t seq, const uint8_t *src_id, bool timed_out)
{
    if (s_app_ack_wait.active && seq == s_app_ack_wait.seq &&
        memcmp(src_id, s_app_ack_wait.dest, AKIRA_MESH_NODE_ID_LEN) == 0) {
        s_app_ack_wait.active = false;
        s_app_ack_wait.acked = !timed_out;
        k_sem_give(&s_app_ack_wait.sem);
    }
}

void mesh_app_dist_module_init(const akira_mesh_config_t *config, akira_mesh_stats_t *stats,
                               akira_mesh_rx_cb_t *rx_cb_ptr, void **rx_ctx_ptr)
{
    memcpy(&s_app_dist.config, config, sizeof(*config));
    s_app_dist.stats = stats;
    s_app_dist.rx_cb_ptr = rx_cb_ptr;
    s_app_dist.rx_ctx_ptr = rx_ctx_ptr;
    s_app_dist.seq_num = 0;
    mesh_seen_reset(&s_app_dist.seen);
    memset(&s_app_dist.app_rx, 0, sizeof(s_app_dist.app_rx));
    s_app_ack_wait.active = false;
    k_sem_init(&s_app_ack_wait.sem, 0, 1);
    s_app_status_wait.active = false;
    k_sem_init(&s_app_status_wait.sem, 0, 1);
    mesh_transport_register_ack_notify(app_dist_on_ack_notify);
}

/* ------------------------------------------------------------------ */
/* Storage destination + reassembly                                    */
/* ------------------------------------------------------------------ */

/* Pick where a transfer of `total_len` bytes will land: SD if available and
 * it actually has room for the WHOLE app, else internal flash if it does,
 * else neither. */
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
 * idempotent already-in-flight no-op). Callers must ack only on true. */
static bool mesh_app_rx_start(const uint8_t *src_id, const struct mesh_app_start_hdr *s)
{
    if (s->magic != MESH_APP_START_MAGIC) return false;
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
    bool stale = (now - s_app_dist.app_rx.last_activity_ms) > CONFIG_AKIRA_MESH_APP_RX_STALE_MS;

    if (s_app_dist.app_rx.active && s_app_dist.app_rx.app_id == s->app_id && !stale) {
        LOG_DBG("Mesh APP_START duplicate for in-flight app_id 0x%08x, ignoring (%u/%u chunks so far)",
                s->app_id, s_app_dist.app_rx.received_count, s_app_dist.app_rx.chunk_count);
        return true;
    }

    bool use_sd;
    const char *tmp_path;
    if (!mesh_app_pick_dest(s->total_len, &use_sd, &tmp_path)) {
        LOG_WRN("Mesh APP_START rejected: no storage with %u free bytes (SD or flash)",
                s->total_len);
        return false;
    }
    fs_manager_delete_file(tmp_path);

    memset(&s_app_dist.app_rx, 0, sizeof(s_app_dist.app_rx));
    s_app_dist.app_rx.active = true;
    s_app_dist.app_rx.use_sd = use_sd;
    s_app_dist.app_rx.app_id = s->app_id;
    s_app_dist.app_rx.total_len = s->total_len;
    s_app_dist.app_rx.chunk_count = s->chunk_count;
    s_app_dist.app_rx.last_activity_ms = now;
    memcpy(s_app_dist.app_rx.origin_id, src_id, AKIRA_MESH_NODE_ID_LEN);
    memcpy(s_app_dist.app_rx.app_name, s->app_name, AKIRA_MESH_APP_NAME_LEN);
    s_app_dist.app_rx.app_name[AKIRA_MESH_APP_NAME_LEN - 1] = '\0';
    strncpy(s_app_dist.app_rx.tmp_path, tmp_path, MESH_APP_TMP_PATH_MAX - 1);
    s_app_dist.app_rx.tmp_path[MESH_APP_TMP_PATH_MAX - 1] = '\0';

    LOG_INF("Mesh app transfer starting: '%s' (%u bytes, %u chunks, dest=%s)",
            s_app_dist.app_rx.app_name, s->total_len, s->chunk_count,
            use_sd ? "SD" : "flash");
    return true;
}

/* Returns true iff this chunk was genuinely accepted (including a repeat of
 * the last one already written). */
static bool mesh_app_rx_chunk(const struct mesh_app_chunk_hdr *ch,
                              const uint8_t *data, size_t data_len)
{
    if (!s_app_dist.app_rx.active) return false;
    uint16_t hdr_crc = crc16_ccitt(0xFFFF, (const uint8_t *)ch,
                                   offsetof(struct mesh_app_chunk_hdr, crc));
    if (crc16_ccitt(hdr_crc, data, data_len) != ch->crc) {
        LOG_WRN("Mesh app chunk dropped: CRC mismatch (header or body)");
        return false;
    }
    if (ch->app_id != s_app_dist.app_rx.app_id) {
        LOG_WRN("Mesh app chunk %u dropped: app_id mismatch", ch->chunk_index);
        return false;
    }
    if (ch->chunk_index >= s_app_dist.app_rx.chunk_count) {
        LOG_WRN("Mesh app chunk %u dropped: index >= chunk_count", ch->chunk_index);
        return false;
    }

    size_t stride = MESH_MAC_PACKET_BUF_SIZE - sizeof(struct mesh_header) -
                    sizeof(struct mesh_app_chunk_hdr);
    size_t off = (size_t)ch->chunk_index * stride;
    if (off + data_len > s_app_dist.app_rx.total_len) {
        LOG_WRN("Mesh app chunk %u dropped: exceeds total_len", ch->chunk_index);
        return false;
    }

    if (ch->chunk_index != s_app_dist.app_rx.received_count) {
        if (s_app_dist.app_rx.received_count > 0 &&
            ch->chunk_index == s_app_dist.app_rx.received_count - 1) {
            s_app_dist.app_rx.last_activity_ms = k_uptime_get_32();
            return true;
        }
        LOG_WRN("Mesh app chunk %u dropped: out of order, expected %u",
                ch->chunk_index, s_app_dist.app_rx.received_count);
        return false;
    }

    ssize_t written = (s_app_dist.app_rx.received_count == 0)
        ? fs_manager_write_file(s_app_dist.app_rx.tmp_path, data, data_len)
        : fs_manager_append_file(s_app_dist.app_rx.tmp_path, data, data_len);
    if (written < 0 || (size_t)written != data_len) {
        LOG_ERR("Mesh app chunk %u write failed: %zd", ch->chunk_index, written);
        return false;
    }
    s_app_dist.app_rx.last_activity_ms = k_uptime_get_32();

    size_t bit = ch->chunk_index;
    s_app_dist.app_rx.received[bit / 8] |= BIT(bit % 8);
    s_app_dist.app_rx.received_count++;

    if (s_app_dist.app_rx.received_count < s_app_dist.app_rx.chunk_count) {
        return true;
    }

    /* Reassembly complete. */
    s_app_dist.app_rx.active = false;
    LOG_INF("Mesh app '%s' reassembled (%u bytes, %u chunks)",
            s_app_dist.app_rx.app_name, s_app_dist.app_rx.total_len,
            s_app_dist.app_rx.chunk_count);

    if (s_app_dist.rx_cb_ptr && *s_app_dist.rx_cb_ptr) {
        uint8_t *buf = akira_malloc_buffer(s_app_dist.app_rx.total_len);
        if (buf) {
            ssize_t n = fs_manager_read_file(s_app_dist.app_rx.tmp_path, buf,
                                             s_app_dist.app_rx.total_len);
            if (n == (ssize_t)s_app_dist.app_rx.total_len) {
                (*s_app_dist.rx_cb_ptr)(s_app_dist.app_rx.origin_id, buf,
                                       s_app_dist.app_rx.total_len, *s_app_dist.rx_ctx_ptr);
            }
            akira_free_buffer(buf);
        } else {
            LOG_WRN("Mesh app '%s' rx_callback skipped: read-back alloc failed",
                    s_app_dist.app_rx.app_name);
        }
    }

#if defined(CONFIG_AKIRA_MESH_APP_AUTO_INSTALL)
    int id = app_manager_register_installed(s_app_dist.app_rx.app_name,
                                            s_app_dist.app_rx.tmp_path,
                                            s_app_dist.app_rx.total_len, APP_SOURCE_MESH,
                                            s_app_dist.app_rx.use_sd);
    if (id < 0) {
        LOG_ERR("Mesh app '%s' auto-install failed: %d", s_app_dist.app_rx.app_name, id);
    }
#else
    fs_manager_delete_file(s_app_dist.app_rx.tmp_path);
#endif
    return true;
}

/* ------------------------------------------------------------------ */
/* Blocking sends (one frame at a time, real end-to-end ack)            */
/* ------------------------------------------------------------------ */

/* Sends one app-distribution frame (START or CHUNK) and blocks for its real
 * end-to-end ack — not just an ack-table slot. A chunk that never actually
 * gets delivered (retries exhaust) must be visible as a failure here, or
 * the whole transfer would report success while the receiver's reassembly
 * can never complete. Blocking per-frame also keeps START-before-chunks
 * ordering intact.
 *
 * A beacon-discovered neighbor has no AODV route yet — so a fresh target
 * needs route discovery (router->resolve()'s own RREQ side effect) before
 * the real send. */
static int mesh_send_app_and_wait(const uint8_t *dest_id, uint8_t msg_type,
                                  const uint8_t *data, size_t len)
{
    const mesh_router_ops_t *router = mesh_router_get_active();
    if (!router) return -ENODEV;

    uint8_t next_hop[AKIRA_MESH_NODE_ID_LEN];
    bool have_route = (router->resolve(dest_id, next_hop) == 0);
    if (!have_route) {
        uint32_t waited_ms = 0;
        const uint32_t route_wait_budget_ms =
            CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS * (CONFIG_AKIRA_MESH_MAX_RETRIES + 1);
        /* resolve() re-fires a RREQ on every miss where nothing is already
         * queued for this dest (nothing is, here — this loop only polls, it
         * never calls queue_pending) — so the poll cadence itself IS the
         * RREQ resend cadence. Must stay at ACK_TIMEOUT_MS, not a tighter
         * gap, or this floods RREQs. */
        while (!have_route && waited_ms < route_wait_budget_ms) {
            k_sleep(K_MSEC(CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS));
            waited_ms += CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS;
            have_route = (router->resolve(dest_id, next_hop) == 0);
        }
        if (!have_route) return -EHOSTUNREACH;
    }

    /* mesh_transport_send_reliable builds the frame, tracks it in Transport's
     * ack_table, and transmits it — the actual seq_num it used is echoed
     * back to us via the ack-notify hook, so arm the wait using the seq_num
     * fill_app_header would have assigned had we built the frame ourselves.
     * Since Transport owns seq_num assignment now, use its return value's
     * side channel instead: mesh_transport_send_reliable increments its own
     * counter internally and there is no way to read the assigned seq_num
     * back from a simple int return — so this function builds and tracks
     * the frame directly via the same primitives Transport itself uses,
     * rather than calling mesh_transport_send_reliable, preserving the
     * ability to arm s_app_ack_wait on the exact seq_num before transmit. */
    uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_app_header(h, msg_type, s_app_dist.config.max_hops, dest_id);
    memcpy(pkt + sizeof(*h), data, len);
    size_t total = sizeof(*h) + len;
    uint16_t seq = h->seq_num;

    s_app_ack_wait.seq = seq;
    memcpy(s_app_ack_wait.dest, dest_id, AKIRA_MESH_NODE_ID_LEN);
    s_app_ack_wait.acked = false;
    s_app_ack_wait.active = true;
    k_sem_reset(&s_app_ack_wait.sem);

    if (s_app_dist.stats) s_app_dist.stats->messages_sent++;
    mesh_mac_send(MESH_MAC_PRIO_LOW, pkt, total);

    int wait_ret = k_sem_take(&s_app_ack_wait.sem,
                              K_MSEC(CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS *
                                     (CONFIG_AKIRA_MESH_MAX_RETRIES + 2)));
    bool acked = s_app_ack_wait.acked;
    s_app_ack_wait.active = false;
    return (wait_ret == 0 && acked) ? 0 : -ETIMEDOUT;
}

/* Best-effort query of the receiver's current reassembly progress for
 * app_id, so a retried distribute_app can skip chunks it already has. */
static int mesh_query_app_rx_progress(const uint8_t *dest_id, uint32_t app_id,
                                      uint8_t *bitmap_out, size_t bitmap_cap,
                                      uint16_t *received_count_out)
{
    const mesh_router_ops_t *router = mesh_router_get_active();
    if (!router) return -ENODEV;
    uint8_t next_hop[AKIRA_MESH_NODE_ID_LEN];
    if (router->resolve(dest_id, next_hop) != 0) return -EHOSTUNREACH;

    uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
    struct mesh_header *h = (struct mesh_header *)pkt;
    fill_app_header(h, AKIRA_MESH_MSG_APP_STATUS_REQ, s_app_dist.config.max_hops, dest_id);
    struct mesh_app_status_req *req = (struct mesh_app_status_req *)(pkt + sizeof(*h));
    req->app_id = app_id;
    size_t total = sizeof(*h) + sizeof(*req);

    s_app_status_wait.app_id = app_id;
    memcpy(s_app_status_wait.dest, dest_id, AKIRA_MESH_NODE_ID_LEN);
    s_app_status_wait.got_resp = false;
    s_app_status_wait.active = true;
    k_sem_reset(&s_app_status_wait.sem);

    mesh_mac_send(MESH_MAC_PRIO_MEDIUM, pkt, total);

    int wait_ret = k_sem_take(&s_app_status_wait.sem, K_MSEC(CONFIG_AKIRA_MESH_ACK_TIMEOUT_MS));

    s_app_status_wait.active = false;
    bool got = s_app_status_wait.got_resp;
    if (got) {
        *received_count_out = s_app_status_wait.received_count;
        size_t n = MIN(s_app_status_wait.bitmap_len, bitmap_cap);
        memcpy(bitmap_out, s_app_status_wait.bitmap, n);
        if (n < bitmap_cap) memset(bitmap_out + n, 0, bitmap_cap - n);
    }
    return (wait_ret == 0 && got) ? 0 : -ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/* RX dispatch entry point                                             */
/* ------------------------------------------------------------------ */

static void transport_style_relay(struct mesh_header *h, const uint8_t *full, size_t len)
{
    if (h->ttl == 0) return;
    const mesh_router_ops_t *router = mesh_router_get_active();
    if (!router) return;
    uint8_t next_hop[AKIRA_MESH_NODE_ID_LEN];
    if (router->resolve(h->dest_id, next_hop) == 0) {
        uint8_t pkt[MESH_MAC_PACKET_BUF_SIZE];
        if (len > sizeof(pkt)) return;
        memcpy(pkt, full, len);
        ((struct mesh_header *)pkt)->ttl = h->ttl - 1;
        if (s_app_dist.stats) s_app_dist.stats->messages_forwarded++;
        mesh_mac_send(MESH_MAC_PRIO_LOW, pkt, len);
        return;
    }
    if (h->ttl <= 1) return;
    router->queue_pending(h->dest_id, full, (uint16_t)len,
                          CONFIG_AKIRA_MESH_LOCAL_REPAIR_TIMEOUT_MS, true);
}

void mesh_app_dist_handle_frame(const uint8_t *buf, size_t len)
{
    if (len < sizeof(struct mesh_header)) return;
    struct mesh_header *h = (struct mesh_header *)buf;
    bool self_dest = is_self(h->dest_id);

    if (!self_dest) {
        /* Relay path only — see s_app_dist.seen's declaration comment for
         * why self-destined frames must skip this. */
        bool dup = mesh_seen_check_and_add(&s_app_dist.seen, h->src_id, h->seq_num);
        if (dup) return;
    }

    const uint8_t *payload = buf + sizeof(*h);
    size_t plen = len - sizeof(*h);

    switch (h->msg_type) {
    case AKIRA_MESH_MSG_APP_START:
        if (self_dest) {
            if (plen >= sizeof(struct mesh_app_start_hdr) &&
                mesh_app_rx_start(h->src_id, (const struct mesh_app_start_hdr *)payload)) {
                uint8_t ackpkt[sizeof(struct mesh_header)];
                struct mesh_header *ah = (struct mesh_header *)ackpkt;
                fill_app_header(ah, AKIRA_MESH_MSG_ACK, 1, h->src_id);
                ah->seq_num = h->seq_num;
                mesh_mac_send(MESH_MAC_PRIO_HIGH, ackpkt, sizeof(ackpkt));
            }
        } else {
            transport_style_relay(h, buf, len);
        }
        break;

    case AKIRA_MESH_MSG_APP_CHUNK:
        if (self_dest) {
            if (plen >= sizeof(struct mesh_app_chunk_hdr)) {
                const struct mesh_app_chunk_hdr *ch = (const struct mesh_app_chunk_hdr *)payload;
                if (mesh_app_rx_chunk(ch, payload + sizeof(*ch), plen - sizeof(*ch))) {
                    uint8_t ackpkt[sizeof(struct mesh_header)];
                    struct mesh_header *ah = (struct mesh_header *)ackpkt;
                    fill_app_header(ah, AKIRA_MESH_MSG_ACK, 1, h->src_id);
                    ah->seq_num = h->seq_num;
                    mesh_mac_send(MESH_MAC_PRIO_HIGH, ackpkt, sizeof(ackpkt));
                }
            }
        } else {
            transport_style_relay(h, buf, len);
        }
        break;

    case AKIRA_MESH_MSG_APP_STATUS_REQ:
        if (self_dest) {
            if (plen < sizeof(struct mesh_app_status_req)) break;
            const struct mesh_app_status_req *req = (const struct mesh_app_status_req *)payload;

            uint8_t resp_pkt[MESH_MAC_PACKET_BUF_SIZE];
            struct mesh_header *rh = (struct mesh_header *)resp_pkt;
            fill_app_header(rh, AKIRA_MESH_MSG_APP_STATUS_RESP, 1, h->src_id);
            struct mesh_app_status_resp *resp =
                (struct mesh_app_status_resp *)(resp_pkt + sizeof(*rh));
            size_t max_bitmap = sizeof(resp_pkt) - sizeof(*rh) - sizeof(*resp);
            size_t bitmap_len = 0;

            resp->app_id = req->app_id;
            if (s_app_dist.app_rx.active && s_app_dist.app_rx.app_id == req->app_id) {
                resp->chunk_count = s_app_dist.app_rx.chunk_count;
                resp->received_count = s_app_dist.app_rx.received_count;
                bitmap_len = MIN(DIV_ROUND_UP(s_app_dist.app_rx.chunk_count, 8), max_bitmap);
                memcpy((uint8_t *)resp + sizeof(*resp), s_app_dist.app_rx.received, bitmap_len);
            } else {
                resp->chunk_count = 0;
                resp->received_count = 0;
            }
            mesh_mac_send(MESH_MAC_PRIO_MEDIUM, resp_pkt, sizeof(*rh) + sizeof(*resp) + bitmap_len);
        } else {
            transport_style_relay(h, buf, len);
        }
        break;

    case AKIRA_MESH_MSG_APP_STATUS_RESP:
        if (self_dest) {
            if (plen < sizeof(struct mesh_app_status_resp)) break;
            const struct mesh_app_status_resp *resp = (const struct mesh_app_status_resp *)payload;
            if (s_app_status_wait.active && resp->app_id == s_app_status_wait.app_id &&
                memcmp(h->src_id, s_app_status_wait.dest, AKIRA_MESH_NODE_ID_LEN) == 0) {
                size_t bitmap_len = plen - sizeof(*resp);
                if (bitmap_len > sizeof(s_app_status_wait.bitmap)) {
                    bitmap_len = sizeof(s_app_status_wait.bitmap);
                }
                memcpy(s_app_status_wait.bitmap, (const uint8_t *)resp + sizeof(*resp), bitmap_len);
                s_app_status_wait.bitmap_len = bitmap_len;
                s_app_status_wait.received_count = resp->received_count;
                s_app_status_wait.got_resp = true;
                k_sem_give(&s_app_status_wait.sem);
            }
        } else {
            transport_style_relay(h, buf, len);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int akira_mesh_get_app_rx_status(akira_mesh_app_rx_status_t *out)
{
    if (!out) return -EINVAL;
    out->active = s_app_dist.app_rx.active;
    out->app_id = s_app_dist.app_rx.app_id;
    memcpy(out->app_name, s_app_dist.app_rx.app_name, AKIRA_MESH_APP_NAME_LEN);
    out->total_len = s_app_dist.app_rx.total_len;
    out->chunk_count = s_app_dist.app_rx.chunk_count;
    out->received_count = s_app_dist.app_rx.received_count;
    out->age_ms = k_uptime_get_32() - s_app_dist.app_rx.last_activity_ms;
    return 0;
}

int akira_mesh_distribute_app(const uint8_t *dest_id, const char *app_name,
                              const uint8_t *app_data, size_t app_len)
{
    if (!dest_id || !app_name || !app_data || app_len == 0) return -EINVAL;

    size_t name_len = strlen(app_name);
    if (name_len == 0 || name_len >= AKIRA_MESH_APP_NAME_LEN) return -ENAMETOOLONG;
    if (app_len > (size_t)CONFIG_AKIRA_APP_MAX_SIZE_KB * 1024) return -EFBIG;

    size_t stride = MESH_MAC_PACKET_BUF_SIZE - sizeof(struct mesh_header) -
                    sizeof(struct mesh_app_chunk_hdr);
    uint16_t chunk_count = (uint16_t)DIV_ROUND_UP(app_len, stride);
    if (chunk_count > MESH_APP_MAX_CHUNKS) return -EFBIG;

    uint32_t app_id = crc32_ieee((const uint8_t *)app_name, name_len);

    /* APP_START */
    struct mesh_app_start_hdr start = {
        .magic = MESH_APP_START_MAGIC,
        .app_id = app_id,
        .total_len = (uint32_t)app_len,
        .chunk_count = chunk_count,
    };
    memcpy(start.app_name, app_name, name_len);
    memset(start.app_name + name_len, 0, AKIRA_MESH_APP_NAME_LEN - name_len);
    start.crc = crc16_ccitt(0xFFFF, (const uint8_t *)&start,
                            offsetof(struct mesh_app_start_hdr, crc));

    int ret = mesh_send_app_and_wait(dest_id, AKIRA_MESH_MSG_APP_START,
                                     (const uint8_t *)&start, sizeof(start));
    if (ret) return ret;

    /* Resume support: skip chunks the receiver already has. Best-effort —
     * a failed query just falls back to sending every chunk. */
    uint8_t resume_bitmap[MESH_APP_BITMAP_BYTES] = {0};
    uint16_t resume_received_count = 0;
    bool have_resume = (mesh_query_app_rx_progress(dest_id, app_id, resume_bitmap,
                                                   sizeof(resume_bitmap),
                                                   &resume_received_count) == 0);

    for (uint16_t i = 0; i < chunk_count; i++) {
        if (have_resume && (resume_bitmap[i / 8] & BIT(i % 8))) {
            continue;
        }
        size_t off = (size_t)i * stride;
        size_t clen = MIN(stride, app_len - off);

        uint8_t pkt[sizeof(struct mesh_app_chunk_hdr) + MESH_ROUTING_PAYLOAD_MAX];
        struct mesh_app_chunk_hdr *ch = (struct mesh_app_chunk_hdr *)pkt;
        ch->app_id = app_id;
        ch->chunk_index = i;
        uint16_t hdr_crc = crc16_ccitt(0xFFFF, (const uint8_t *)ch,
                                       offsetof(struct mesh_app_chunk_hdr, crc));
        memcpy(pkt + sizeof(*ch), app_data + off, clen);
        ch->crc = crc16_ccitt(hdr_crc, app_data + off, clen);

        int cret = -EBUSY;
        for (int attempt = 0; attempt < CONFIG_AKIRA_MESH_APP_TX_RETRIES; attempt++) {
            cret = mesh_send_app_and_wait(dest_id, AKIRA_MESH_MSG_APP_CHUNK, pkt, sizeof(*ch) + clen);
            if (cret != -EBUSY) break;
            k_msleep(CONFIG_AKIRA_MESH_APP_TX_GAP_MS);
        }
        if (cret) return cret;
        k_msleep(CONFIG_AKIRA_MESH_APP_TX_GAP_MS);
    }

    if (s_app_dist.stats) s_app_dist.stats->apps_distributed++;
    return 0;
}
