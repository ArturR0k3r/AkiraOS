#ifndef AKIRA_MESH_ROUTER_H
#define AKIRA_MESH_ROUTER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
#include "mesh_crypto.h"
#endif

#define MESH_ROUTER_NAME_MAX 16

typedef struct {
    /* Bring the router up/down. Called by mesh_router_acquire/_release. */
    int  (*start)(void);
    int  (*stop)(void);
    /* Resolve next hop for dest. 0 + next_hop_out on a cache hit;
     * -EHOSTUNREACH on a miss — the implementation triggers its own
     * discovery as a side effect (AODV: fires a RREQ if one isn't already
     * in flight for this dest), the caller just polls resolve() again
     * later rather than triggering discovery itself. */
    int  (*resolve)(const uint8_t *dest_id, uint8_t *next_hop_out);
    /* Router-owned control frames mesh_dispatch() doesn't parse itself
     * (AODV: RREQ/RREP/RERR) are handed here unparsed. */
    void (*handle_control_frame)(const uint8_t *buf, size_t len);
    /* Buffer a payload awaiting route discovery, keyed by dest, using the
     * router's own pending-route bookkeeping (so retry/backoff/timeout is
     * driven by the same tick() as discovery itself, not a second copy of
     * the same state in Transport). is_local_repair: true when the caller
     * is a relay whose existing route through itself just broke (worth a
     * broadcast RERR if the repair times out); false when the caller is
     * the original source and never had a route to begin with (nothing to
     * announce on timeout). Returns a slot index or -EBUSY. */
    int  (*queue_pending)(const uint8_t *dest, const uint8_t *payload,
                          uint16_t len, uint32_t timeout_ms, bool is_local_repair);
    /* Transport gives up on a destination (ack retries exhausted) — lets
     * the router invalidate/repair without Transport knowing routing
     * internals. */
    void (*notify_unreachable)(const uint8_t *dest_id);
    /* Periodic tick for the router's own timers (GC, RREQ retry, etc). */
    void (*tick)(uint32_t now_ms);
} mesh_router_ops_t;

int mesh_router_register(const char *name, const mesh_router_ops_t *ops);
int mesh_router_unregister(const char *name);
/* Stops the currently-active router (if any — its state is discarded, the
 * two implementations aren't assumed compatible), then starts this one. */
int mesh_router_acquire(const char *name);
int mesh_router_release(void);
const mesh_router_ops_t *mesh_router_get_active(void);

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
/* Shared handshake-completion step for any router implementation that
 * piggybacks an X3DH-lite handshake on its own control frames (AODV's
 * RREQ/RREP today): ECDH(my_priv, peer_pub) then install the derived
 * session for peer_id via Transport. Zeroes the shared secret internally.
 * Factored out so a second router doesn't have to reimplement this exact
 * ECDH-then-install sequence.
 * @return 0 on success, whatever mesh_crypto_p256_ecdh() returns on failure.
 */
int mesh_router_derive_and_install_session(const uint8_t *my_priv,
                                           const uint8_t *local_id,
                                           const uint8_t *peer_pub,
                                           const uint8_t *peer_id,
                                           uint32_t now_ms, uint32_t lifetime_ms);
#endif

#endif /* AKIRA_MESH_ROUTER_H */
