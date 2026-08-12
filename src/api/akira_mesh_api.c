/**
 * @file akira_mesh_api.c
 * @brief WASM native export bridge for AkiraMesh
 *
 * Bridges the push-based akira_mesh_register_rx_callback() into a pollable
 * queue, since a WASM app has no thread to receive an async callback on
 * (mirrors the akira_rf_api.c recv_pop bridge for the RF RX daemon).
 */

#include "akira_mesh_api.h"
#include "akira_rf_api.h"
#include "connectivity/akira_mesh.h"
#include <runtime/security.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_mesh_api, CONFIG_AKIRA_LOG_LEVEL);

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#define MESH_API_RX_QUEUE_DEPTH 8

struct mesh_api_rx_packet {
    uint8_t  src_id[AKIRA_MESH_NODE_ID_LEN];
    uint8_t  data[AKIRA_MESH_API_MAX_PAYLOAD];
    uint16_t len;
};

K_MSGQ_DEFINE(s_mesh_api_rx_msgq, sizeof(struct mesh_api_rx_packet),
              MESH_API_RX_QUEUE_DEPTH, 4);

static void mesh_api_rx_cb(const uint8_t *src_id, const uint8_t *data,
                           size_t len, void *user_data)
{
    ARG_UNUSED(user_data);
    struct mesh_api_rx_packet pkt;
    memcpy(pkt.src_id, src_id, AKIRA_MESH_NODE_ID_LEN);
    pkt.len = (uint16_t)MIN(len, sizeof(pkt.data));
    memcpy(pkt.data, data, pkt.len);

    if (k_msgq_put(&s_mesh_api_rx_msgq, &pkt, K_NO_WAIT) == -ENOMSG) {
        struct mesh_api_rx_packet discard;
        k_msgq_get(&s_mesh_api_rx_msgq, &discard, K_NO_WAIT);
        k_msgq_put(&s_mesh_api_rx_msgq, &pkt, K_NO_WAIT);
    }
}

/* Manual app-pointer translation + bounds check, mirroring the raw
 * capture/replay helpers in akira_rf_api.c. */
static void *mesh_api_translate(wasm_exec_env_t exec_env, uint32_t app_ptr,
                                 uint32_t len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) {
        return NULL;
    }
    void *native = wasm_runtime_addr_app_to_native(inst, app_ptr);
    if (!native || !wasm_runtime_validate_native_addr(inst, native, len)) {
        return NULL;
    }
    return native;
}

int akira_native_mesh_init(wasm_exec_env_t exec_env, int32_t node_id,
                            const char *name, int32_t role,
                            uint32_t beacon_interval_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (node_id < 0 || node_id > 0xFF) {
        return -EINVAL;
    }
    if (role < AKIRA_MESH_ROLE_NODE || role > AKIRA_MESH_ROLE_PROVISIONER) {
        return -EINVAL;
    }

    akira_mesh_config_t cfg = {0};
    cfg.node_id[AKIRA_MESH_NODE_ID_LEN - 1] = (uint8_t)node_id;
    if (name) {
        strncpy(cfg.node_name, name, sizeof(cfg.node_name) - 1);
    } else {
        snprintf(cfg.node_name, sizeof(cfg.node_name), "akira-%02x", (uint8_t)node_id);
    }
    cfg.role = (akira_mesh_role_t)role;
    /* Only LR2021 (LoRa) is registered on this hardware — see the "On this
     * hardware the mesh binds to the LR2021" doc comment in akira_api.h. */
    cfg.transport = AKIRA_MESH_TRANSPORT_LORA;
    cfg.max_hops = AKIRA_MESH_MAX_HOPS;
    cfg.beacon_interval_ms = beacon_interval_ms ? beacon_interval_ms : 5000;

    akira_rf_release_all();

    int ret = akira_mesh_init(&cfg);
    if (ret) {
        return ret;
    }

    akira_mesh_register_rx_callback(mesh_api_rx_cb, NULL);
    return 0;
}

int akira_native_mesh_start(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);
    return akira_mesh_start();
}

int akira_native_mesh_stop(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);
    return akira_mesh_stop();
}

int akira_native_mesh_send(wasm_exec_env_t exec_env, uint32_t dest_id_ptr,
                            uint32_t data_ptr, uint32_t data_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (data_len == 0 || data_len > AKIRA_MESH_API_MAX_PAYLOAD) {
        return -EINVAL;
    }

    uint8_t *dest_id = mesh_api_translate(exec_env, dest_id_ptr, AKIRA_MESH_NODE_ID_LEN);
    uint8_t *data = mesh_api_translate(exec_env, data_ptr, data_len);
    if (!dest_id || !data) {
        return -EFAULT;
    }

    return akira_mesh_send(dest_id, data, data_len);
}

int akira_native_mesh_broadcast(wasm_exec_env_t exec_env, uint32_t data_ptr,
                                 uint32_t data_len, int32_t max_hops)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (data_len == 0 || data_len > AKIRA_MESH_API_MAX_PAYLOAD) {
        return -EINVAL;
    }
    if (max_hops < 0 || max_hops > 0xFF) {
        return -EINVAL;
    }

    uint8_t *data = mesh_api_translate(exec_env, data_ptr, data_len);
    if (!data) {
        return -EFAULT;
    }

    return akira_mesh_broadcast(data, data_len, (uint8_t)max_hops);
}

int akira_native_mesh_recv_pop(wasm_exec_env_t exec_env, uint32_t src_id_out_ptr,
                                uint32_t buf_ptr, uint32_t max_len,
                                uint32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (max_len == 0) {
        return -EINVAL;
    }

    uint8_t *src_id_out = mesh_api_translate(exec_env, src_id_out_ptr, AKIRA_MESH_NODE_ID_LEN);
    uint8_t *buf = mesh_api_translate(exec_env, buf_ptr, max_len);
    if (!src_id_out || !buf) {
        return -EFAULT;
    }

    struct mesh_api_rx_packet pkt;
    k_timeout_t t = (timeout_ms == 0) ? K_NO_WAIT : K_MSEC(timeout_ms);
    int ret = k_msgq_get(&s_mesh_api_rx_msgq, &pkt, t);
    if (ret < 0) {
        return ret;
    }

    memcpy(src_id_out, pkt.src_id, AKIRA_MESH_NODE_ID_LEN);
    size_t copy = MIN(pkt.len, max_len);
    memcpy(buf, pkt.data, copy);
    return (int)copy;
}

int akira_native_mesh_get_nodes(wasm_exec_env_t exec_env, uint32_t buf_ptr,
                                 uint32_t max_nodes)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (max_nodes == 0) {
        return -EINVAL;
    }
    max_nodes = MIN(max_nodes, (uint32_t)AKIRA_MESH_MAX_NODES);

    uint8_t *buf = mesh_api_translate(exec_env, buf_ptr,
                                       max_nodes * sizeof(akira_mesh_node_info_t));
    if (!buf) {
        return -EFAULT;
    }

    akira_mesh_node_info_t nodes[AKIRA_MESH_MAX_NODES];
    int count = akira_mesh_get_nodes(nodes, max_nodes);
    if (count < 0) {
        return count;
    }

    memcpy(buf, nodes, (size_t)count * sizeof(akira_mesh_node_info_t));
    return count;
}

int akira_native_mesh_get_stats(wasm_exec_env_t exec_env, uint32_t buf_ptr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    uint8_t *buf = mesh_api_translate(exec_env, buf_ptr, sizeof(akira_mesh_stats_t));
    if (!buf) {
        return -EFAULT;
    }

    akira_mesh_stats_t stats;
    int ret = akira_mesh_get_stats(&stats);
    if (ret) {
        return ret;
    }

    memcpy(buf, &stats, sizeof(stats));
    return 0;
}

int akira_native_mesh_distribute_app(wasm_exec_env_t exec_env, uint32_t dest_id_ptr,
                                      const char *app_name,
                                      uint32_t data_ptr, uint32_t data_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MESH, -EPERM);

    if (!app_name || data_len == 0) {
        return -EINVAL;
    }

    uint8_t *dest_id = mesh_api_translate(exec_env, dest_id_ptr, AKIRA_MESH_NODE_ID_LEN);
    uint8_t *data = mesh_api_translate(exec_env, data_ptr, data_len);
    if (!dest_id || !data) {
        return -EFAULT;
    }

    return akira_mesh_distribute_app(dest_id, app_name, data, data_len);
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
