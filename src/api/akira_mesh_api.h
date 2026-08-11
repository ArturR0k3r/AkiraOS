/**
 * @file akira_mesh_api.h
 * @brief AkiraMesh WASM native export declarations
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_MESH_API_H
#define AKIRA_MESH_API_H

#include <stdint.h>
#include <stddef.h>
#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

/* Max user-payload bytes per send/broadcast/recv, leaving room for the
 * AkiraMesh header inside mesh_manager's internal packet buffer. */
#define AKIRA_MESH_API_MAX_PAYLOAD 200

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM native export functions (with capability checks) */
int akira_native_mesh_init(wasm_exec_env_t exec_env, int32_t node_id,
                            const char *name, int32_t role,
                            uint32_t beacon_interval_ms);
int akira_native_mesh_start(wasm_exec_env_t exec_env);
int akira_native_mesh_stop(wasm_exec_env_t exec_env);
int akira_native_mesh_send(wasm_exec_env_t exec_env, uint32_t dest_id_ptr,
                            uint32_t data_ptr, uint32_t data_len);
int akira_native_mesh_broadcast(wasm_exec_env_t exec_env, uint32_t data_ptr,
                                 uint32_t data_len, int32_t max_hops);
int akira_native_mesh_recv_pop(wasm_exec_env_t exec_env, uint32_t src_id_out_ptr,
                                uint32_t buf_ptr, uint32_t max_len,
                                uint32_t timeout_ms);
int akira_native_mesh_get_nodes(wasm_exec_env_t exec_env, uint32_t buf_ptr,
                                 uint32_t max_nodes);
int akira_native_mesh_get_stats(wasm_exec_env_t exec_env, uint32_t buf_ptr);
int akira_native_mesh_distribute_app(wasm_exec_env_t exec_env, uint32_t dest_id_ptr,
                                      const char *app_name,
                                      uint32_t data_ptr, uint32_t data_len);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_MESH_API_H */
