/**
 * @file akira_mesh_api.h
 * @brief AkiraMesh WASM native API — declarations for akira_export_api.c.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_MESH_API_H
#define AKIRA_MESH_API_H

#include <stdint.h>
#include <wasm_export.h>

#ifdef __cplusplus
extern "C" {
#endif

int akira_native_mesh_init(wasm_exec_env_t exec_env, int node_id,
                           const char *name, int role,
                           uint32_t beacon_interval_ms);
int akira_native_mesh_start(wasm_exec_env_t exec_env);
int akira_native_mesh_stop(wasm_exec_env_t exec_env);
int akira_native_mesh_send(wasm_exec_env_t exec_env, void *dest_id,
                           void *data, uint32_t len);
int akira_native_mesh_broadcast(wasm_exec_env_t exec_env, void *data,
                                uint32_t len, int max_hops);
int akira_native_mesh_recv_pop(wasm_exec_env_t exec_env, void *src_id_out,
                               void *buf, uint32_t max_len,
                               uint32_t timeout_ms);
int akira_native_mesh_get_nodes(wasm_exec_env_t exec_env, void *buf,
                                uint32_t max_nodes);
int akira_native_mesh_get_stats(wasm_exec_env_t exec_env, void *buf);
int akira_native_mesh_distribute_app(wasm_exec_env_t exec_env, void *dest_id,
                                     const char *app_name, void *data,
                                     uint32_t len);

/**
 * @brief Tear down any app-owned mesh session.
 *
 * Called by the runtime when a WASM app exits so a crashed or force-killed
 * app does not leave the radio acquired and the RX queue allocated.
 */
void akira_mesh_api_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MESH_API_H */
