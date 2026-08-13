#include "mesh_routing.h"
#include "mesh_mac.h"
#include <string.h>
#include <errno.h>

/* ---- duplicate suppression ---- */

void mesh_seen_reset(struct seen_cache *c)
{
    memset(c, 0, sizeof(*c));
}

bool mesh_seen_check(const struct seen_cache *c, const uint8_t *src_id, uint16_t seq_num)
{
    for (uint16_t i = 0; i < CONFIG_AKIRA_MESH_SEEN_CACHE; i++) {
        if (c->entries[i].used &&
            c->entries[i].seq_num == seq_num &&
            memcmp(c->entries[i].src_id, src_id, AKIRA_MESH_NODE_ID_LEN) == 0) {
            return true;
        }
    }
    return false;
}

void mesh_seen_add(struct seen_cache *c, const uint8_t *src_id, uint16_t seq_num)
{
    struct seen_entry *e = &c->entries[c->head];
    memcpy(e->src_id, src_id, AKIRA_MESH_NODE_ID_LEN);
    e->seq_num = seq_num;
    e->used = true;
    c->head = (c->head + 1) % CONFIG_AKIRA_MESH_SEEN_CACHE;
}

bool mesh_seen_check_and_add(struct seen_cache *c, const uint8_t *src_id, uint16_t seq_num)
{
    if (mesh_seen_check(c, src_id, seq_num)) {
        return true;
    }
    mesh_seen_add(c, src_id, seq_num);
    return false;
}

/* ---- AODV seq compare + route table ---- */

void mesh_route_reset(struct route_table *t) { memset(t, 0, sizeof(*t)); }

static struct route_entry *route_find(struct route_table *t, const uint8_t *dest)
{
    for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_ROUTES; i++) {
        if (t->e[i].valid && memcmp(t->e[i].dest_id, dest, AKIRA_MESH_NODE_ID_LEN) == 0) {
            return &t->e[i];
        }
    }
    return NULL;
}

struct route_entry *mesh_route_lookup(struct route_table *t, const uint8_t *dest, uint32_t now_ms)
{
    struct route_entry *r = route_find(t, dest);
    if (!r) {
        return NULL;
    }
    if ((int32_t)(r->expiry_ms - now_ms) <= 0) {
        return NULL;   /* expired */
    }
    r->last_used_ms = now_ms;
    return r;
}

bool mesh_route_install(struct route_table *t, const uint8_t *dest, const uint8_t *next_hop,
                        uint8_t hop_count, uint16_t dest_seq, uint32_t now_ms, uint32_t expiry_ms)
{
    struct route_entry *r = route_find(t, dest);
    if (r) {
        bool newer = mesh_seq_newer(dest_seq, r->dest_seq);
        bool same_seq_better = (dest_seq == r->dest_seq) && (hop_count < r->hop_count);
        if (!newer && !same_seq_better) {
            return false;
        }
    } else {
        for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_ROUTES; i++) {
            if (!t->e[i].valid) { r = &t->e[i]; break; }
        }
        if (!r) {
            /* Table full: evict the least-recently-used entry instead of
             * refusing new destinations forever once CONFIG_AKIRA_MESH_MAX_ROUTES
             * is reached. */
            r = &t->e[0];
            for (int i = 1; i < CONFIG_AKIRA_MESH_MAX_ROUTES; i++) {
                if ((int32_t)(t->e[i].last_used_ms - r->last_used_ms) < 0) {
                    r = &t->e[i];
                }
            }
        }
    }
    memcpy(r->dest_id, dest, AKIRA_MESH_NODE_ID_LEN);
    memcpy(r->next_hop, next_hop, AKIRA_MESH_NODE_ID_LEN);
    r->hop_count = hop_count;
    r->dest_seq = dest_seq;
    r->expiry_ms = expiry_ms;
    r->last_used_ms = now_ms;   /* install counts as a fresh use */
    r->valid = true;
    return true;
}

bool mesh_route_invalidate(struct route_table *t, const uint8_t *dest)
{
    struct route_entry *r = route_find(t, dest);
    if (r && r->valid) {
        r->valid = false;
        return true;
    }
    return false;
}

void mesh_route_touch(struct route_table *t, const uint8_t *dest, uint32_t now_ms,
                      uint32_t new_expiry_ms)
{
    struct route_entry *r = route_find(t, dest);
    if (r && r->valid) { r->expiry_ms = new_expiry_ms; r->last_used_ms = now_ms; }
}

void mesh_route_gc(struct route_table *t, uint32_t now_ms)
{
    for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_ROUTES; i++) {
        if (t->e[i].valid && (int32_t)(t->e[i].expiry_ms - now_ms) <= 0) {
            t->e[i].valid = false;
        }
    }
}

/* ---- pending-ACK table + retransmit decision ---- */

void mesh_ack_reset(struct ack_table *t) { memset(t, 0, sizeof(*t)); }

int mesh_ack_add(struct ack_table *t, uint16_t seq, const uint8_t *dest,
                 const uint8_t *payload, uint16_t len, uint32_t now_ms,
                 uint32_t deadline_ms)
{
    if (len > MESH_ROUTING_PAYLOAD_MAX) {
        return -EMSGSIZE;
    }
    int slot = -1;
    for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_PENDING_ACKS; i++) {
        if (!t->e[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        /* Full: evict the oldest in-flight send. A busy mesh degrades by
         * losing its least-recently-touched pending send instead of
         * refusing all new reliable sends. */
        slot = 0;
        for (int i = 1; i < CONFIG_AKIRA_MESH_MAX_PENDING_ACKS; i++) {
            if (t->e[i].last_used_ms < t->e[slot].last_used_ms) {
                slot = i;
            }
        }
    }
    struct pending_ack *e = &t->e[slot];
    e->seq_num = seq; e->len = len; e->retries = 0;
    e->deadline_ms = deadline_ms; e->last_used_ms = now_ms; e->active = true;
    e->sent_at_ms = now_ms;
    memcpy(e->dest_id, dest, AKIRA_MESH_NODE_ID_LEN);
    memcpy(e->payload, payload, len);
    return slot;
}

bool mesh_ack_clear(struct ack_table *t, uint16_t seq, const uint8_t *dest,
                    uint32_t now_ms, uint32_t *elapsed_ms, uint8_t *retries_out)
{
    for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_PENDING_ACKS; i++) {
        struct pending_ack *e = &t->e[i];
        if (e->active && e->seq_num == seq &&
            memcmp(e->dest_id, dest, AKIRA_MESH_NODE_ID_LEN) == 0) {
            if (elapsed_ms) { *elapsed_ms = now_ms - e->sent_at_ms; }
            if (retries_out) { *retries_out = e->retries; }
            e->active = false;
            return true;
        }
    }
    return false;
}

mesh_ack_action_t mesh_ack_tick(struct pending_ack *e, uint32_t now_ms, uint32_t timeout_ms)
{
    if (!e->active) {
        return MESH_ACK_SKIP;
    }
    if ((int32_t)(e->deadline_ms - now_ms) > 0) {
        return MESH_ACK_SKIP;
    }
    if (e->retries >= CONFIG_AKIRA_MESH_MAX_RETRIES) {
        return MESH_ACK_GIVE_UP;
    }
    e->retries++;
    e->deadline_ms = now_ms + timeout_ms;
    e->last_used_ms = now_ms;
    return MESH_ACK_RETRANSMIT;
}

/* ---- pending-route queue ---- */

void mesh_pr_reset(struct pending_route_q *q) { memset(q, 0, sizeof(*q)); }

int mesh_pr_add(struct pending_route_q *q, const uint8_t *dest,
                const uint8_t *payload, uint16_t len, uint32_t now_ms,
                uint32_t deadline_ms, bool is_local_repair)
{
    if (len > MESH_ROUTING_PAYLOAD_MAX) {
        return -EMSGSIZE;
    }
    int slot = -1;
    for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_PENDING_ROUTES; i++) {
        if (!q->e[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < CONFIG_AKIRA_MESH_MAX_PENDING_ROUTES; i++) {
            if (q->e[i].last_used_ms < q->e[slot].last_used_ms) {
                slot = i;
            }
        }
    }
    struct pending_route *e = &q->e[slot];
    memcpy(e->dest_id, dest, AKIRA_MESH_NODE_ID_LEN);
    memcpy(e->payload, payload, len);
    e->len = len; e->deadline_ms = deadline_ms; e->last_used_ms = now_ms;
    e->rreq_retries = 0;
    /* First retry check fires one backoff interval after the initial RREQ,
     * not at now_ms — mesh_pr_tick's own doubling picks up from here. */
    e->next_rreq_ms = now_ms + (mesh_mac_ack_timeout_ms() << 1);
    e->is_local_repair = is_local_repair;
    e->active = true;
    return slot;
}

struct pending_route *mesh_pr_next_for_dest(struct pending_route_q *q, const uint8_t *dest)
{
    for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_PENDING_ROUTES; i++) {
        if (q->e[i].active &&
            memcmp(q->e[i].dest_id, dest, AKIRA_MESH_NODE_ID_LEN) == 0) {
            return &q->e[i];
        }
    }
    return NULL;
}

void mesh_pr_clear_slot(struct pending_route *e) { e->active = false; }

void mesh_pr_gc(struct pending_route_q *q, uint32_t now_ms,
                void (*on_drop)(const uint8_t *dest, bool is_local_repair, void *ctx),
                void *ctx)
{
    for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_PENDING_ROUTES; i++) {
        struct pending_route *e = &q->e[i];
        if (e->active && (int32_t)(e->deadline_ms - now_ms) <= 0) {
            if (on_drop) {
                on_drop(e->dest_id, e->is_local_repair, ctx);
            }
            e->active = false;
        }
    }
}

void mesh_pr_tick(struct pending_route_q *q, uint32_t now_ms,
                  void (*on_retry)(const uint8_t *dest, void *ctx), void *ctx)
{
    for (int i = 0; i < CONFIG_AKIRA_MESH_MAX_PENDING_ROUTES; i++) {
        struct pending_route *e = &q->e[i];
        if (!e->active || e->rreq_retries >= CONFIG_AKIRA_MESH_RREQ_MAX_RETRIES) {
            continue;
        }
        if ((int32_t)(e->next_rreq_ms - now_ms) > 0) {
            continue;
        }
        e->rreq_retries++;
        uint32_t backoff = mesh_mac_ack_timeout_ms() << (e->rreq_retries + 1);
        e->next_rreq_ms = now_ms + backoff;
        if (on_retry) {
            on_retry(e->dest_id, ctx);
        }
    }
}
