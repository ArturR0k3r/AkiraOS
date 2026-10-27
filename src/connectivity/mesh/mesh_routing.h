#ifndef AKIRA_MESH_ROUTING_H
#define AKIRA_MESH_ROUTING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "connectivity/akira_mesh.h"

#define MESH_ROUTING_PAYLOAD_MAX 256

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
    bool     valid;
};

struct route_table { struct route_entry e[CONFIG_AKIRA_MESH_MAX_ROUTES]; };

void mesh_route_reset(struct route_table *t);
/* valid + unexpired entry for dest, or NULL */
struct route_entry *mesh_route_lookup(struct route_table *t, const uint8_t *dest, uint32_t now_ms);
/* install/refresh only if incoming seq is newer-or-equal-with-shorter-hops.
 * Returns true if the table changed. */
bool mesh_route_install(struct route_table *t, const uint8_t *dest, const uint8_t *next_hop,
                        uint8_t hop_count, uint16_t dest_seq, uint32_t expiry_ms);
void mesh_route_invalidate(struct route_table *t, const uint8_t *dest);
/* mark expired entries invalid */
void mesh_route_gc(struct route_table *t, uint32_t now_ms);

/* ---- pending-ACK table + retransmit decision ---- */
struct pending_ack {
    uint16_t seq_num;
    uint8_t  dest_id[AKIRA_MESH_NODE_ID_LEN];
    uint16_t len;
    uint8_t  retries;            /* initial send = retry 0 */
    uint32_t deadline_ms;
    uint8_t  payload[MESH_ROUTING_PAYLOAD_MAX];
    bool     active;
};

struct ack_table { struct pending_ack e[CONFIG_AKIRA_MESH_MAX_PENDING_ACKS]; };

typedef enum { MESH_ACK_SKIP, MESH_ACK_RETRANSMIT, MESH_ACK_GIVE_UP } mesh_ack_action_t;

void mesh_ack_reset(struct ack_table *t);
/* Add a tracked packet. -EBUSY if table full, else slot index. */
int  mesh_ack_add(struct ack_table *t, uint16_t seq, const uint8_t *dest,
                  const uint8_t *payload, uint16_t len, uint32_t deadline_ms);
/* Clear by (seq,dest). true if found. */
bool mesh_ack_clear(struct ack_table *t, uint16_t seq, const uint8_t *dest);
/* Decide action for one entry at now_ms; on RETRANSMIT bumps retries + new deadline. */
mesh_ack_action_t mesh_ack_tick(struct pending_ack *e, uint32_t now_ms, uint32_t timeout_ms);

/* ---- pending-route queue ---- */
struct pending_route {
    uint8_t  dest_id[AKIRA_MESH_NODE_ID_LEN];
    uint16_t len;
    uint32_t deadline_ms;
    uint8_t  payload[MESH_ROUTING_PAYLOAD_MAX];
    bool     active;
};

struct pending_route_q { struct pending_route e[CONFIG_AKIRA_MESH_MAX_PENDING_ROUTES]; };

void mesh_pr_reset(struct pending_route_q *q);
int  mesh_pr_add(struct pending_route_q *q, const uint8_t *dest,
                 const uint8_t *payload, uint16_t len, uint32_t deadline_ms);
/* Find next active entry for dest (for flushing on RREP). NULL if none.
 * Caller clears via mesh_pr_clear_slot after sending. */
struct pending_route *mesh_pr_next_for_dest(struct pending_route_q *q, const uint8_t *dest);
void mesh_pr_clear_slot(struct pending_route *e);
/* Drop entries past deadline; invokes cb(dest) for each dropped (may be NULL). */
void mesh_pr_gc(struct pending_route_q *q, uint32_t now_ms,
                void (*on_drop)(const uint8_t *dest, void *ctx), void *ctx);

#endif /* AKIRA_MESH_ROUTING_H */
