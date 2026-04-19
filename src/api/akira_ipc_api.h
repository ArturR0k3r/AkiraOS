/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_ipc_api.h
 * @brief Inter-App Messaging WASM API — named topic pub/sub
 *
 * Requires AKIRA_CAP_IPC in the app manifest.
 *
 * Manifest entry:  "capabilities": ["ipc"]
 *
 * The calling app's name is resolved automatically from the exec_env so
 * msg_recv() cannot eavesdrop on another app's subscription.
 */

#ifndef AKIRA_IPC_API_H
#define AKIRA_IPC_API_H

#include <stdint.h>
#include <wasm_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Subscribe to a named topic
 * @param topic  Null-terminated topic name
 * @return 0 on success, -ENOMEM if tables full, -EINVAL on bad args
 */
int akira_native_msg_subscribe(wasm_exec_env_t exec_env, const char *topic);

/**
 * @brief Unsubscribe from a named topic
 * @param topic  Topic name
 * @return 0 on success, -ENOENT if not subscribed
 */
int akira_native_msg_unsubscribe(wasm_exec_env_t exec_env, const char *topic);

/**
 * @brief Publish a message to all subscribers of a topic
 * @param topic     Topic name
 * @param data_ptr  WASM pointer to payload
 * @param len       Payload length (max CONFIG_AKIRA_IPC_MSG_MAX_SIZE)
 * @return Number of subscribers that received the message, negative on error
 */
int akira_native_msg_publish(wasm_exec_env_t exec_env,
                             const char *topic,
                             uint32_t data_ptr, uint32_t len);

/**
 * @brief Receive next message from a topic (blocking with timeout)
 * @param topic       Topic name
 * @param buf_ptr     WASM pointer to receive buffer
 * @param buf_len     Buffer capacity
 * @param timeout_ms  Timeout in ms (0 = non-blocking, -1 = wait forever)
 * @return Bytes received, -EAGAIN on timeout, negative on error
 */
int akira_native_msg_recv(wasm_exec_env_t exec_env,
                          const char *topic,
                          uint32_t buf_ptr, uint32_t buf_len,
                          int32_t timeout_ms);

/**
 * @brief Non-blocking receive (equivalent to msg_recv with timeout=0)
 * @param topic    Topic name
 * @param buf_ptr  WASM pointer to receive buffer
 * @param buf_len  Buffer capacity
 * @return Bytes received, -EAGAIN if no message pending, negative on error
 */
int akira_native_msg_try_recv(wasm_exec_env_t exec_env,
                              const char *topic,
                              uint32_t buf_ptr, uint32_t buf_len);

/**
 * @brief Query number of pending messages in a subscription queue
 * @param topic  Topic name
 * @return Pending count (>= 0), -ENOENT if not subscribed, negative on error
 */
int akira_native_msg_pending(wasm_exec_env_t exec_env, const char *topic);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_IPC_API_H */
