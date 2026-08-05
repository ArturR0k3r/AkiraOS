/**
 * @file akira_mesh_api.c
 * @brief AkiraMesh WASM native API — bridges apps to the AkiraMesh stack.
 *
 * The host stack (src/connectivity/mesh/) owns route discovery, per-hop ACK
 * and retransmit, duplicate suppression and E2E crypto. This module only
 * translates between the WASM ABI and akira_mesh.h, and owns the RX queue
 * that turns the stack's push callback into the app's pull-style
 * mesh_recv_pop().
 *
 * The RX queue lives in PSRAM (akira_malloc_buffer) — it is far too large for
 * the ~79 KB DRAM system heap that BLE and WiFi keep near-exhausted.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 * @stability experimental
 * @since 1.6
 */

#include "akira_mesh_api.h"

#include <connectivity/akira_mesh.h>
#include <runtime/security.h>
#include <lib/mem_helper.h>

#ifdef CONFIG_AKIRA_MODULE_RF
#include "akira_rf_api.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

LOG_MODULE_REGISTER(akira_mesh_api, CONFIG_AKIRA_LOG_LEVEL);

/* Must match AKIRA_MESH_MAX_PAYLOAD in AkiraSDK/include/akira_api.h. */
#define MESH_API_MAX_PAYLOAD 200
#define MESH_API_RX_DEPTH    CONFIG_AKIRA_WASM_MESH_RX_QUEUE_DEPTH

/** Validate a WASM pointer/length pair, or bail with -EFAULT. */
#define WASM_ADDR_CHECK(inst, ptr, len)                                      \
    do {                                                                     \
        if (!(ptr) || !wasm_runtime_validate_native_addr((inst), (ptr),      \
                                                         (len))) {           \
            return -EFAULT;                                                  \
        }                                                                    \
    } while (0)

/* ---- ABI mirrors --------------------------------------------------------
 * The SDK documents these as fixed 52/24-byte little-endian layouts. They are
 * field-identical to the host structs, but assert it rather than assume it:
 * a silent layout drift here corrupts app memory.
 */

typedef struct {
    uint8_t  node_id[AKIRA_MESH_NODE_ID_LEN];
    char     name[AKIRA_MESH_APP_NAME_LEN];
    int32_t  role;
    uint8_t  hop_count;
    int8_t   rssi;
    uint8_t  lqi;
    uint32_t last_seen;
} wasm_mesh_node_t;

typedef struct {
    uint32_t nodes_discovered;
    uint32_t messages_sent;
    uint32_t messages_received;
    uint32_t messages_forwarded;
    uint32_t routes_active;
    uint32_t apps_distributed;
} wasm_mesh_stats_t;

BUILD_ASSERT(sizeof(wasm_mesh_node_t) == 52, "mesh node ABI drift");
BUILD_ASSERT(sizeof(wasm_mesh_node_t) == sizeof(akira_mesh_node_info_t),
             "mesh node no longer mirrors the host struct");
BUILD_ASSERT(offsetof(wasm_mesh_node_t, name) ==
                 offsetof(akira_mesh_node_info_t, name) &&
             offsetof(wasm_mesh_node_t, role) ==
                 offsetof(akira_mesh_node_info_t, role) &&
             offsetof(wasm_mesh_node_t, last_seen) ==
                 offsetof(akira_mesh_node_info_t, last_seen),
             "mesh node field offsets diverged from the host struct");
BUILD_ASSERT(sizeof(wasm_mesh_stats_t) == 24, "mesh stats ABI drift");
BUILD_ASSERT(sizeof(wasm_mesh_stats_t) == sizeof(akira_mesh_stats_t),
             "mesh stats no longer mirrors the host struct");

/* ---- RX queue ----------------------------------------------------------- */

struct mesh_rx_item {
    uint8_t  src_id[AKIRA_MESH_NODE_ID_LEN];
    uint16_t len;
    uint8_t  data[MESH_API_MAX_PAYLOAD];
};

static struct k_msgq s_rx_q;
static char         *s_rx_buf;      /* PSRAM backing store for s_rx_q */
static bool          s_rx_ready;
static bool          s_session;     /* an app currently owns the mesh */
static uint32_t      s_rx_dropped;

static K_MUTEX_DEFINE(s_lock);

/* Runs on the mesh RX thread — never blocks. On a full queue the oldest item
 * is discarded so a slow app sees fresh traffic rather than a stalled backlog.
 */
static void mesh_api_rx_cb(const uint8_t *src_id, const uint8_t *data,
                           size_t len, void *user_data)
{
    ARG_UNUSED(user_data);

    if (!s_rx_ready || !data || len == 0) {
        return;
    }

    struct mesh_rx_item item;
    item.len = (uint16_t)MIN(len, (size_t)MESH_API_MAX_PAYLOAD);
    memcpy(item.data, data, item.len);
    if (src_id) {
        memcpy(item.src_id, src_id, AKIRA_MESH_NODE_ID_LEN);
    } else {
        memset(item.src_id, 0, AKIRA_MESH_NODE_ID_LEN);
    }

    if (k_msgq_put(&s_rx_q, &item, K_NO_WAIT) != 0) {
        struct mesh_rx_item drop;
        if (k_msgq_get(&s_rx_q, &drop, K_NO_WAIT) == 0) {
            s_rx_dropped++;
        }
        (void)k_msgq_put(&s_rx_q, &item, K_NO_WAIT);
    }
}

static int rx_queue_alloc(void)
{
    if (s_rx_buf) {
        k_msgq_purge(&s_rx_q);
        s_rx_ready = true;
        return 0;
    }

    size_t bytes = sizeof(struct mesh_rx_item) * MESH_API_RX_DEPTH;
    s_rx_buf = akira_malloc_buffer(bytes);
    if (!s_rx_buf) {
        LOG_ERR("mesh: RX queue alloc failed (%zu bytes)", bytes);
        return -ENOMEM;
    }
    k_msgq_init(&s_rx_q, s_rx_buf, sizeof(struct mesh_rx_item),
                MESH_API_RX_DEPTH);
    s_rx_ready = true;
    return 0;
}

static void rx_queue_free(void)
{
    s_rx_ready = false;
    if (s_rx_buf) {
        k_msgq_purge(&s_rx_q);
        akira_free_buffer(s_rx_buf);
        s_rx_buf = NULL;
    }
}

/* ---- Transport selection ------------------------------------------------
 * Purely capability-based, in preference order — no board or chip is named
 * here. The mesh acquires the first free radio matching a mask; a target with
 * none of them fails with -ENODEV.
 *
 * Order rationale, in generic terms: a dedicated sub-GHz transceiver is
 * normally idle and exclusively available, so it is tried first. BLE is tried
 * last because acquiring it forces the shared BT stack into BT_MODE_MESH and
 * evicts whatever else was using it.
 */
static const uint32_t s_transport_caps[] = {
    AKIRA_MESH_CAPS_SUBGHZ,
#if defined(CONFIG_AKIRA_MESH_TRANSPORT_802154)
    AKIRA_MESH_CAPS_802154,
#endif
#if defined(CONFIG_AKIRA_MESH_TRANSPORT_BLE)
    AKIRA_MESH_CAPS_BLE,
#endif
};

/* ---- Natives ------------------------------------------------------------ */

/* mesh_init — sig "(i$ii)i" */
int akira_native_mesh_init(wasm_exec_env_t exec_env, int node_id,
                           const char *name, int role,
                           uint32_t beacon_interval_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (node_id < 0 || node_id > 255) {
        return -EINVAL;
    }
    if (role < AKIRA_MESH_ROLE_NODE || role > AKIRA_MESH_ROLE_PROVISIONER) {
        return -EINVAL;
    }

    k_mutex_lock(&s_lock, K_FOREVER);

    if (s_session) {
        k_mutex_unlock(&s_lock);
        return -EALREADY;
    }

    /* The mesh needs exclusive ownership of a radio. Drop any rf_* claim the
     * app made earlier in the same session — mixing rf_* and mesh_* is
     * documented as unsupported, so this is a convenience, not a policy hole.
     */
#ifdef CONFIG_AKIRA_MODULE_RF
    (void)akira_rf_deinit();
#endif

    akira_mesh_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id[AKIRA_MESH_NODE_ID_LEN - 1] = (uint8_t)node_id;
    cfg.role = (akira_mesh_role_t)role;
    cfg.max_hops = CONFIG_AKIRA_MESH_MAX_HOPS;
    cfg.beacon_interval_ms = (beacon_interval_ms == 0) ? 5000
                                                       : beacon_interval_ms;

    if (name && name[0] != '\0') {
        strncpy(cfg.node_name, name, sizeof(cfg.node_name) - 1);
    } else {
        snprintf(cfg.node_name, sizeof(cfg.node_name), "akira-%02X",
                 (unsigned)node_id);
    }

    int ret = -ENODEV;
    for (size_t i = 0; i < ARRAY_SIZE(s_transport_caps); i++) {
        cfg.transport_caps = s_transport_caps[i];
        ret = akira_mesh_init(&cfg);
        if (ret == 0) {
            break;
        }
        /* -ENODEV just means "no radio matches this mask" — keep looking.
         * Any other error is a real failure and must not be papered over by
         * silently binding a different transport. */
        if (ret != -ENODEV) {
            break;
        }
    }
    if (ret != 0) {
        k_mutex_unlock(&s_lock);
        LOG_ERR("mesh_init failed: %d", ret);
        return ret;
    }

    ret = rx_queue_alloc();
    if (ret != 0) {
        akira_mesh_stop();
        k_mutex_unlock(&s_lock);
        return ret;
    }
    s_rx_dropped = 0;

    akira_mesh_register_rx_callback(mesh_api_rx_cb, NULL);
    s_session = true;

    k_mutex_unlock(&s_lock);
    LOG_INF("mesh_init: node 0x%02X '%s' role=%d", (unsigned)node_id,
            cfg.node_name, role);
    return 0;
}

/* mesh_start — sig "()i" */
int akira_native_mesh_start(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);
    if (!s_session) {
        return -ENODEV;
    }
    return akira_mesh_start();
}

/* mesh_stop — sig "()i" */
int akira_native_mesh_stop(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    k_mutex_lock(&s_lock, K_FOREVER);
    if (!s_session) {
        k_mutex_unlock(&s_lock);
        return 0;
    }
    /* Detach the callback before freeing the queue it writes into. */
    akira_mesh_register_rx_callback(NULL, NULL);
    int ret = akira_mesh_stop();
    rx_queue_free();
    s_session = false;
    k_mutex_unlock(&s_lock);
    return ret;
}

/* mesh_send — sig "(**~)i" */
int akira_native_mesh_send(wasm_exec_env_t exec_env, void *dest_id,
                           void *data, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (len == 0 || len > MESH_API_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, dest_id, AKIRA_MESH_NODE_ID_LEN);
    WASM_ADDR_CHECK(inst, data, len);

    if (!s_session) {
        return -ENODEV;
    }
    return akira_mesh_send((const uint8_t *)dest_id, (const uint8_t *)data,
                           (size_t)len);
}

/* mesh_broadcast — sig "(*~i)i" */
int akira_native_mesh_broadcast(wasm_exec_env_t exec_env, void *data,
                                uint32_t len, int max_hops)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (len == 0 || len > MESH_API_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    if (max_hops <= 0 || max_hops > AKIRA_MESH_MAX_HOPS) {
        max_hops = AKIRA_MESH_MAX_HOPS;
    }
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, data, len);

    if (!s_session) {
        return -ENODEV;
    }
    return akira_mesh_broadcast((const uint8_t *)data, (size_t)len,
                                (uint8_t)max_hops);
}

/* mesh_recv_pop — sig "(**~i)i" */
int akira_native_mesh_recv_pop(wasm_exec_env_t exec_env, void *src_id_out,
                               void *buf, uint32_t max_len,
                               uint32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (max_len == 0) {
        return -EINVAL;
    }
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, src_id_out, AKIRA_MESH_NODE_ID_LEN);
    WASM_ADDR_CHECK(inst, buf, max_len);

    if (!s_session || !s_rx_ready) {
        return -ENODEV;
    }

    struct mesh_rx_item item;
    if (k_msgq_get(&s_rx_q, &item, K_MSEC(timeout_ms)) != 0) {
        return -EAGAIN;
    }

    memcpy(src_id_out, item.src_id, AKIRA_MESH_NODE_ID_LEN);
    uint32_t n = MIN(max_len, (uint32_t)item.len);
    memcpy(buf, item.data, n);
    return (int)n;
}

/* mesh_get_nodes — sig "(*i)i". Deliberately NOT "*~": max_nodes counts
 * records, not bytes, so WAMR's own length check would under-validate by 52×.
 * The byte-accurate check is done below. */
int akira_native_mesh_get_nodes(wasm_exec_env_t exec_env, void *buf,
                                uint32_t max_nodes)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (max_nodes == 0 || max_nodes > AKIRA_MESH_MAX_NODES) {
        return -EINVAL;
    }
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, buf, max_nodes * sizeof(wasm_mesh_node_t));

    if (!s_session) {
        return -ENODEV;
    }

    /* akira_mesh_get_nodes() always copies from the table head, so it cannot
     * be windowed — it writes straight into the (already bounds-checked) WASM
     * buffer. Safe only because the two record layouts are asserted identical
     * above; no staging copy and no temporary allocation is needed. */
    return akira_mesh_get_nodes((akira_mesh_node_info_t *)buf,
                                (size_t)max_nodes);
}

/* mesh_get_stats — sig "(*)i" */
int akira_native_mesh_get_stats(wasm_exec_env_t exec_env, void *buf)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, buf, sizeof(wasm_mesh_stats_t));

    akira_mesh_stats_t stats;
    int ret = akira_mesh_get_stats(&stats);
    if (ret != 0) {
        return ret;
    }

    wasm_mesh_stats_t *out = (wasm_mesh_stats_t *)buf;
    out->nodes_discovered  = stats.nodes_discovered;
    out->messages_sent     = stats.messages_sent;
    out->messages_received = stats.messages_received;
    out->messages_forwarded = stats.messages_forwarded;
    out->routes_active     = stats.routes_active;
    out->apps_distributed  = stats.apps_distributed;
    return 0;
}

/* mesh_distribute_app — sig "(*$*~)i" */
int akira_native_mesh_distribute_app(wasm_exec_env_t exec_env, void *dest_id,
                                     const char *app_name, void *data,
                                     uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (!app_name || len == 0) {
        return -EINVAL;
    }
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, dest_id, AKIRA_MESH_NODE_ID_LEN);
    WASM_ADDR_CHECK(inst, data, len);

    if (!s_session) {
        return -ENODEV;
    }
    /* The host stack has no per-destination distribution yet and returns
     * -ENOSYS; dest_id is validated above so the ABI stays stable once
     * chunking lands. */
    return akira_mesh_distribute_app(app_name, (const uint8_t *)data,
                                     (size_t)len);
}

void akira_mesh_api_cleanup(void)
{
    k_mutex_lock(&s_lock, K_FOREVER);
    if (s_session) {
        akira_mesh_register_rx_callback(NULL, NULL);
        akira_mesh_stop();
        rx_queue_free();
        s_session = false;
        LOG_INF("mesh: session released on app exit");
    }
    k_mutex_unlock(&s_lock);
}
