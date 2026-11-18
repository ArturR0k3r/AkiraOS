#ifndef AKIRA_MESH_SESSION_H
#define AKIRA_MESH_SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "connectivity/akira_mesh.h"
#include "mesh_crypto.h"

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)

/* Per-peer E2E session: derived once via ECDH+HKDF off the RREQ/RREP
 * handshake, cached with a TTL. Simple expiry (not a ratchet) — re-derive
 * by repeating the same RREQ/RREP round trip. */
struct session_entry {
    uint8_t  peer_id[AKIRA_MESH_NODE_ID_LEN];
    uint8_t  enc_key[MESH_CRYPTO_SESSION_KEY_LEN];
    uint8_t  mac_key[MESH_CRYPTO_MAC_KEY_LEN];
    uint32_t expiry_ms;
    bool     valid;
};

struct session_table { struct session_entry e[CONFIG_AKIRA_MESH_MAX_SESSIONS]; };

void mesh_session_reset(struct session_table *t);

/* valid + unexpired session for peer_id, or NULL. */
struct session_entry *mesh_session_lookup(struct session_table *t,
                                          const uint8_t *peer_id, uint32_t now_ms);

/* Derive enc_key/mac_key from a raw ECDH shared secret via HKDF-SHA256
 * (salt = the two node_ids in deterministic byte order, so both sides of
 * the handshake compute the identical salt independently) and install/
 * replace the cached entry for peer_id. Zeroes shared_secret's local copy
 * internally; caller is still responsible for zeroing its own copy. */
void mesh_session_install(struct session_table *t,
                          const uint8_t *local_id, const uint8_t *peer_id,
                          const uint8_t *shared_secret, size_t shared_len,
                          uint32_t now_ms, uint32_t ttl_ms);

/* Mark expired entries invalid (mirrors mesh_route_gc). */
void mesh_session_gc(struct session_table *t, uint32_t now_ms);

/* Extend expiry_ms for peer_id's entry if one exists (mirrors
 * mesh_route_touch) — keeps a long-running exchange's session alive past
 * its original TTL as long as traffic keeps flowing, without re-deriving
 * key material. No-op if no session exists for peer_id. */
void mesh_session_touch(struct session_table *t, const uint8_t *peer_id,
                        uint32_t new_expiry_ms);

#endif /* CONFIG_AKIRA_MESH_E2E_CRYPTO */

#endif /* AKIRA_MESH_SESSION_H */
