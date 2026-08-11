#ifndef AKIRA_MESH_ROUTING_H
#define AKIRA_MESH_ROUTING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "connectivity/akira_mesh.h"

#define MESH_ROUTING_PAYLOAD_MAX 256

/* Shared wire header, used by every mesh_*.c file that builds or parses a
 * frame. src_id/dest_id always mean "true originator"/"true final
 * destination" and never change as a frame is relayed hop-by-hop — DATA/
 * ACK/dedup/E2E-crypto all depend on that. (AODV's relay_id, carried inside
 * its own control payloads, is the separate "immediate previous hop" notion
 * relaying needs — see mesh_aodv.c.) */
struct __packed mesh_header {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  ttl;
    uint8_t  src_id[AKIRA_MESH_NODE_ID_LEN];
    uint8_t  dest_id[AKIRA_MESH_NODE_ID_LEN];
    uint16_t seq_num;
};

/* ---- duplicate suppression ---- */
struct seen_entry { uint8_t src_id[AKIRA_MESH_NODE_ID_LEN]; uint16_t seq_num; bool used; };

struct seen_cache {
    struct seen_entry entries[CONFIG_AKIRA_MESH_SEEN_CACHE];
    uint16_t head;
};

void mesh_seen_reset(struct seen_cache *c);
/* true if (src,seq) already present; else records it and returns false */
bool mesh_seen_check_and_add(struct seen_cache *c, const uint8_t *src_id, uint16_t seq_num);

/* ---- AODV seq compare + route table ---- */
/* signed-difference seq compare: true if a is strictly newer than b */
static inline bool mesh_seq_newer(uint16_t a, uint16_t b) { return (int16_t)(a - b) > 0; }

struct route_entry {
    uint8_t  dest_id[AKIRA_MESH_NODE_ID_LEN];
    uint8_t  next_hop[AKIRA_MESH_NODE_ID_LEN];
    uint8_t  hop_count;
    uint16_t dest_seq;
    uint32_t expiry_ms;
    uint32_t last_used_ms;   /* for LRU eviction when the table is full */
    bool     valid;
};

struct route_table { struct route_entry e[CONFIG_AKIRA_MESH_MAX_ROUTES]; };

void mesh_route_reset(struct route_table *t);
/* valid + unexpired entry for dest, or NULL */
struct route_entry *mesh_route_lookup(struct route_table *t, const uint8_t *dest, uint32_t now_ms);
/* install/refresh only if incoming seq is newer-or-equal-with-shorter-hops.
 * Returns true if the table changed. Evicts the least-recently-used entry
 * instead of failing when the table is full and dest isn't already present. */
bool mesh_route_install(struct route_table *t, const uint8_t *dest, const uint8_t *next_hop,
                        uint8_t hop_count, uint16_t dest_seq, uint32_t expiry_ms);
/* @return true if a valid entry for dest existed and was just invalidated,
 * false if there was nothing to invalidate (already invalid, or never
 * existed) — the caller uses this to decide whether propagating a RERR one
 * more hop is warranted. */
bool mesh_route_invalidate(struct route_table *t, const uint8_t *dest);
/* Extend expiry on a still-valid entry (route actively in use) — no-op if
 * absent/expired, does not install. */
void mesh_route_touch(struct route_table *t, const uint8_t *dest, uint32_t new_expiry_ms);
/* mark expired entries invalid */
void mesh_route_gc(struct route_table *t, uint32_t now_ms);

/* ---- pending-ACK table + retransmit decision ---- */
struct pending_ack {
    uint16_t seq_num;
    uint8_t  dest_id[AKIRA_MESH_NODE_ID_LEN];
    uint16_t len;
    uint8_t  retries;            /* initial send = retry 0 */
    uint32_t deadline_ms;
    uint32_t last_used_ms;       /* for LRU eviction when the table is full */
    uint8_t  payload[MESH_ROUTING_PAYLOAD_MAX];
    bool     active;
};

struct ack_table { struct pending_ack e[CONFIG_AKIRA_MESH_MAX_PENDING_ACKS]; };

typedef enum { MESH_ACK_SKIP, MESH_ACK_RETRANSMIT, MESH_ACK_GIVE_UP } mesh_ack_action_t;

void mesh_ack_reset(struct ack_table *t);
/* Add a tracked packet. Evicts the least-recently-used in-flight entry
 * instead of failing when the table is full. Returns the slot index. */
int  mesh_ack_add(struct ack_table *t, uint16_t seq, const uint8_t *dest,
                  const uint8_t *payload, uint16_t len, uint32_t now_ms,
                  uint32_t deadline_ms);
/* Clear by (seq,dest). true if found. */
bool mesh_ack_clear(struct ack_table *t, uint16_t seq, const uint8_t *dest);
/* Decide action for one entry at now_ms; on RETRANSMIT bumps retries + new deadline. */
mesh_ack_action_t mesh_ack_tick(struct pending_ack *e, uint32_t now_ms, uint32_t timeout_ms);

/* ---- pending-route queue ---- */
struct pending_route {
    uint8_t  dest_id[AKIRA_MESH_NODE_ID_LEN];
    uint16_t len;
    uint32_t deadline_ms;
    uint32_t last_used_ms;      /* for LRU eviction when the queue is full */
    uint8_t  rreq_retries;      /* RREQ attempts fired so far for this entry */
    uint32_t next_rreq_ms;      /* when to fire the next retry */
    bool     is_local_repair;   /* true if an intermediate relay queued this (a
                                  * route through it existed and broke); false
                                  * if the originating source never had one at
                                  * all — only the former is worth a broadcast
                                  * RERR when it eventually times out. */
    uint8_t  payload[MESH_ROUTING_PAYLOAD_MAX];
    bool     active;
};

struct pending_route_q { struct pending_route e[CONFIG_AKIRA_MESH_MAX_PENDING_ROUTES]; };

void mesh_pr_reset(struct pending_route_q *q);
int  mesh_pr_add(struct pending_route_q *q, const uint8_t *dest,
                 const uint8_t *payload, uint16_t len, uint32_t now_ms,
                 uint32_t deadline_ms, bool is_local_repair);
/* Find next active entry for dest (for flushing on RREP). NULL if none.
 * Caller clears via mesh_pr_clear_slot after sending. */
struct pending_route *mesh_pr_next_for_dest(struct pending_route_q *q, const uint8_t *dest);
void mesh_pr_clear_slot(struct pending_route *e);
/* Drop entries past deadline; invokes cb(dest, is_local_repair, ctx) for each
 * dropped (may be NULL). */
void mesh_pr_gc(struct pending_route_q *q, uint32_t now_ms,
                void (*on_drop)(const uint8_t *dest, bool is_local_repair, void *ctx),
                void *ctx);
/* Scans for entries whose next_rreq_ms has passed; for each, bumps
 * rreq_retries, doubles the retry interval (capped by
 * CONFIG_AKIRA_MESH_RREQ_MAX_RETRIES total retries), and invokes
 * on_retry(dest, ctx) once so the caller re-fires that dest's RREQ. Entries
 * that have exhausted their retry budget are left alone — mesh_pr_gc's
 * overall deadline still drops them eventually. */
void mesh_pr_tick(struct pending_route_q *q, uint32_t now_ms,
                  void (*on_retry)(const uint8_t *dest, void *ctx), void *ctx);

#endif /* AKIRA_MESH_ROUTING_H */
