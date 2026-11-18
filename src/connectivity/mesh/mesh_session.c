/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mesh_session.h"
#include <string.h>

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)

void mesh_session_reset(struct session_table *t)
{
    memset(t, 0, sizeof(*t));
}

struct session_entry *mesh_session_lookup(struct session_table *t,
                                          const uint8_t *peer_id, uint32_t now_ms)
{
    for (size_t i = 0; i < CONFIG_AKIRA_MESH_MAX_SESSIONS; i++) {
        struct session_entry *e = &t->e[i];
        if (e->valid && (int32_t)(e->expiry_ms - now_ms) > 0 &&
            memcmp(e->peer_id, peer_id, AKIRA_MESH_NODE_ID_LEN) == 0) {
            return e;
        }
    }
    return NULL;
}

void mesh_session_install(struct session_table *t,
                          const uint8_t *local_id, const uint8_t *peer_id,
                          const uint8_t *shared_secret, size_t shared_len,
                          uint32_t now_ms, uint32_t ttl_ms)
{
    /* Deterministic salt regardless of which side derives first: sort the
     * two node_ids by byte value. */
    uint8_t salt[2 * AKIRA_MESH_NODE_ID_LEN];
    if (memcmp(local_id, peer_id, AKIRA_MESH_NODE_ID_LEN) < 0) {
        memcpy(salt, local_id, AKIRA_MESH_NODE_ID_LEN);
        memcpy(salt + AKIRA_MESH_NODE_ID_LEN, peer_id, AKIRA_MESH_NODE_ID_LEN);
    } else {
        memcpy(salt, peer_id, AKIRA_MESH_NODE_ID_LEN);
        memcpy(salt + AKIRA_MESH_NODE_ID_LEN, local_id, AKIRA_MESH_NODE_ID_LEN);
    }
    static const char info[] = "AkiraMesh-v1-session";

    uint8_t okm[MESH_CRYPTO_SESSION_KEY_LEN + MESH_CRYPTO_MAC_KEY_LEN];
    mesh_crypto_hkdf_sha256(shared_secret, shared_len, salt, sizeof(salt),
                            (const uint8_t *)info, sizeof(info) - 1,
                            okm, sizeof(okm));

    /* Find an existing entry for this peer (re-key), else a free slot, else
     * evict the entry with the soonest expiry. */
    struct session_entry *slot = NULL;
    for (size_t i = 0; i < CONFIG_AKIRA_MESH_MAX_SESSIONS; i++) {
        struct session_entry *e = &t->e[i];
        if (e->valid && memcmp(e->peer_id, peer_id, AKIRA_MESH_NODE_ID_LEN) == 0) {
            slot = e;
            break;
        }
    }
    if (!slot) {
        for (size_t i = 0; i < CONFIG_AKIRA_MESH_MAX_SESSIONS; i++) {
            if (!t->e[i].valid) {
                slot = &t->e[i];
                break;
            }
        }
    }
    if (!slot) {
        slot = &t->e[0];
        for (size_t i = 1; i < CONFIG_AKIRA_MESH_MAX_SESSIONS; i++) {
            if ((int32_t)(t->e[i].expiry_ms - slot->expiry_ms) < 0) {
                slot = &t->e[i];
            }
        }
    }

    memcpy(slot->peer_id, peer_id, AKIRA_MESH_NODE_ID_LEN);
    memcpy(slot->enc_key, okm, MESH_CRYPTO_SESSION_KEY_LEN);
    memcpy(slot->mac_key, okm + MESH_CRYPTO_SESSION_KEY_LEN, MESH_CRYPTO_MAC_KEY_LEN);
    slot->expiry_ms = now_ms + ttl_ms;
    slot->valid = true;

    memset(salt, 0, sizeof(salt));
    memset(okm, 0, sizeof(okm));
}

void mesh_session_touch(struct session_table *t, const uint8_t *peer_id,
                        uint32_t new_expiry_ms)
{
    for (size_t i = 0; i < CONFIG_AKIRA_MESH_MAX_SESSIONS; i++) {
        struct session_entry *e = &t->e[i];
        if (e->valid && memcmp(e->peer_id, peer_id, AKIRA_MESH_NODE_ID_LEN) == 0) {
            e->expiry_ms = new_expiry_ms;
            return;
        }
    }
}

void mesh_session_gc(struct session_table *t, uint32_t now_ms)
{
    for (size_t i = 0; i < CONFIG_AKIRA_MESH_MAX_SESSIONS; i++) {
        struct session_entry *e = &t->e[i];
        if (e->valid && (int32_t)(e->expiry_ms - now_ms) <= 0) {
            memset(e, 0, sizeof(*e));
        }
    }
}

#endif /* CONFIG_AKIRA_MESH_E2E_CRYPTO */
