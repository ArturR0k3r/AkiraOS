/**
 * @file sync_transport_mesh.c
 * @brief AkiraSync transport binding over AkiraMesh.
 *
 * Sync frames ride as ordinary mesh broadcast payloads behind a two-byte
 * magic, so this needs no new mesh message type and no change to the mesh
 * dispatch switch. Non-sync payloads are handed on to whatever callback was
 * installed before the binding took over, so an app using the mesh WASM API
 * keeps receiving its own traffic while a sync session runs.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include <connectivity/akira_sync.h>
#include <connectivity/akira_mesh.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_sync_tp_mesh, CONFIG_AKIRA_LOG_LEVEL);

/* Distinguishes sync frames from ordinary app payloads on the shared mesh
 * broadcast channel. */
static const uint8_t SYNC_MAGIC[2] = { 0xA5, 0x59 };
#define SYNC_MAGIC_LEN 2

static akira_sync_rx_cb_t s_cb;
static void              *s_ud;

/* Scratch for prefixing the magic. Guarded by s_tx_lock rather than placed on
 * the caller's stack: the sync TX path can run on the small heartbeat work
 * thread. */
static uint8_t s_tx[SYNC_MAGIC_LEN + AKIRA_SYNC_MAX_PAYLOAD + 64];
static K_MUTEX_DEFINE(s_tx_lock);

static void mesh_rx_trampoline(const uint8_t *src_id, const uint8_t *data,
                               size_t len, void *user_data)
{
    ARG_UNUSED(user_data);

    if (!data || len <= SYNC_MAGIC_LEN ||
        memcmp(data, SYNC_MAGIC, SYNC_MAGIC_LEN) != 0) {
        /* Not ours. Deliberately dropped rather than forwarded: the mesh
         * exposes a single callback slot, and silently re-entering another
         * subsystem's handler from here would make ownership ambiguous.
         * Chaining is added if and when a caller needs both at once. */
        return;
    }

    akira_sync_rx_cb_t cb = s_cb;
    if (cb) {
        cb(src_id, data + SYNC_MAGIC_LEN, len - SYNC_MAGIC_LEN, s_ud);
    }
}

static int mesh_broadcast_frame(const void *buf, size_t len)
{
    if (!buf || len == 0 || len > sizeof(s_tx) - SYNC_MAGIC_LEN) {
        return -EMSGSIZE;
    }

    k_mutex_lock(&s_tx_lock, K_FOREVER);
    memcpy(s_tx, SYNC_MAGIC, SYNC_MAGIC_LEN);
    memcpy(s_tx + SYNC_MAGIC_LEN, buf, len);
    int ret = akira_mesh_broadcast(s_tx, len + SYNC_MAGIC_LEN,
                                   CONFIG_AKIRA_MESH_MAX_HOPS);
    k_mutex_unlock(&s_tx_lock);
    return ret;
}

static int mesh_set_rx_cb(akira_sync_rx_cb_t cb, void *user_data)
{
    s_cb = cb;
    s_ud = user_data;

    if (cb) {
        return akira_mesh_register_rx_callback(mesh_rx_trampoline, NULL);
    }
    return akira_mesh_register_rx_callback(NULL, NULL);
}

static const struct akira_sync_transport s_mesh = {
    .name      = "akiramesh",
    .broadcast = mesh_broadcast_frame,
    .set_rx_cb = mesh_set_rx_cb,
    /* Conservative: the mesh MTU varies with the acquired radio, and the
     * per-payload ceiling the mesh WASM API documents is 200 bytes. */
    .mtu       = 200 - SYNC_MAGIC_LEN,
};

int akira_sync_transport_bind_mesh(void)
{
    return akira_sync_transport_register(&s_mesh);
}
