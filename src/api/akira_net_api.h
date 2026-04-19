/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_NET_API_H
#define AKIRA_NET_API_H

/**
 * @file akira_net_api.h
 * @brief WASM native function signatures for the Network API bridge.
 *
 * Required manifest capability: "network"
 *
 * Typical WASM app flow — TCP client:
 *
 *   // Allocate ring buffers in WASM memory
 *   static uint8_t tx_buf[2048];
 *   static uint8_t rx_buf[2048];
 *
 *   int h = net_open(NET_TYPE_TCP);
 *   net_tx_bind(h, tx_buf, sizeof(tx_buf));
 *   net_rx_bind(h, rx_buf, sizeof(rx_buf));
 *   net_connect(h, "192.168.1.10", 8080);
 *
 *   uint8_t evt_buf[4];
 *   while (net_event_pop(evt_buf, sizeof(evt_buf)) != NET_EVT_CONNECTED) {
 *       delay(5000);
 *   }
 *
 *   // Write to TX ring, then flush
 *   net_ring_write(tx_buf, sizeof(tx_buf), "Hello", 5);
 *   net_tx_flush(h);
 *
 *   // Wait for data
 *   while (1) {
 *       int evt = net_event_pop(evt_buf, sizeof(evt_buf));
 *       if (evt == NET_EVT_DATA_READY) {
 *           uint8_t msg[256];
 *           int len = net_ring_read(rx_buf, sizeof(rx_buf), msg, sizeof(msg));
 *           // process msg
 *       }
 *   }
 *
 * Typical WASM app flow — TCP server:
 *
 *   int srv = net_open(NET_TYPE_TCP);
 *   net_bind(srv, 9000);
 *   net_listen(srv, 4);
 *
 *   // poll net_event_pop() for NET_EVT_ACCEPT — extra field = new handle
 *   // then bind TX/RX rings on the accepted handle and exchange data
 */

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a new TCP or UDP socket.
 * @param exec_env  WAMR execution environment.
 * @param type      NET_TYPE_TCP (0) or NET_TYPE_UDP (1).
 * @return Stream handle (>=0) on success; negative errno on failure.
 */
int akira_native_net_open(wasm_exec_env_t exec_env, int32_t type);

/**
 * @brief Initiate an async connect to a remote host:port.
 *
 * Returns immediately; NET_EVT_CONNECTED (or NET_EVT_ERROR) is posted
 * asynchronously. For UDP, sets the default peer (no handshake).
 *
 * @param exec_env  WAMR execution environment.
 * @param handle    Stream handle.
 * @param host      Null-terminated hostname or IPv4 dotted-decimal string.
 * @param port      Remote port (1–65535).
 * @return 0 if queued; negative errno on failure.
 */
int akira_native_net_connect(wasm_exec_env_t exec_env, int32_t handle,
			     const char *host, int32_t port);

/**
 * @brief Bind a socket to a local port.
 * @param exec_env  WAMR execution environment.
 * @param handle    Stream handle.
 * @param port      Local port (0 = OS-assigned).
 * @return 0 on success; negative errno on failure.
 */
int akira_native_net_bind(wasm_exec_env_t exec_env, int32_t handle,
			  int32_t port);

/**
 * @brief Mark a TCP socket as listening.
 * @param exec_env  WAMR execution environment.
 * @param handle    Stream handle.
 * @param backlog   Accept queue depth.
 * @return 0 on success; negative errno on failure.
 */
int akira_native_net_listen(wasm_exec_env_t exec_env, int32_t handle,
			    int32_t backlog);

/**
 * @brief Close and release a socket stream.
 * @param exec_env  WAMR execution environment.
 * @param handle    Stream handle.
 * @return 0 on success; negative errno on failure.
 */
int akira_native_net_close(wasm_exec_env_t exec_env, int32_t handle);

/**
 * @brief Bind a WASM linear-memory buffer as the TX ring for a stream.
 *
 * The first NET_RING_HDR_SIZE bytes of the buffer become the ring header;
 * the host writes the data-area capacity there.
 * After a successful call the WASM app owns the write_idx; the host owns read_idx.
 *
 * @param exec_env   WAMR execution environment.
 * @param handle     Stream handle.
 * @param wasm_ptr   WASM app-address of the buffer.
 * @param total_size Total buffer size in bytes (must be > NET_RING_HDR_SIZE = 16).
 * @return 0 on success; negative errno on failure.
 */
int akira_native_net_tx_bind(wasm_exec_env_t exec_env, int32_t handle,
			     int32_t wasm_ptr, int32_t total_size);

/**
 * @brief Bind a WASM linear-memory buffer as the RX ring for a stream.
 *
 * After a successful call the host owns write_idx; the WASM app owns read_idx.
 *
 * @param exec_env   WAMR execution environment.
 * @param handle     Stream handle.
 * @param wasm_ptr   WASM app-address of the buffer.
 * @param total_size Total buffer size in bytes (must be > NET_RING_HDR_SIZE = 16).
 * @return 0 on success; negative errno on failure.
 */
int akira_native_net_rx_bind(wasm_exec_env_t exec_env, int32_t handle,
			     int32_t wasm_ptr, int32_t total_size);

/**
 * @brief Drain the TX ring immediately and send pending bytes over the socket.
 *
 * The poll thread drains automatically on each 10 ms tick. Call this for
 * lower-latency sends without waiting for the next tick.
 *
 * @param exec_env  WAMR execution environment.
 * @param handle    Stream handle.
 * @return Bytes sent on success; negative errno on failure.
 */
int akira_native_net_tx_flush(wasm_exec_env_t exec_env, int32_t handle);

/**
 * @brief Pop the next network event from the event queue (non-blocking).
 *
 * Event buffer layout:
 *   Byte 0      : event type (NET_EVT_*)
 *   Byte 1      : stream handle
 *   Bytes 2–3   : extra (uint16 LE) — new handle for ACCEPT, errno for ERROR
 *
 * @param exec_env  WAMR execution environment.
 * @param buf_ptr   WASM address of a ≥4-byte destination buffer.
 * @param len       Buffer capacity in bytes.
 * @return Event type (NET_EVT_*; >0) if available; 0 if queue is empty.
 */
int akira_native_net_event_pop(wasm_exec_env_t exec_env,
			       uint32_t buf_ptr, uint32_t len);

/**
 * @brief Get the IPv4 address of the default network interface.
 *
 * Writes a null-terminated dotted-decimal string (e.g. "192.168.1.42") into
 * the WASM buffer. Returns 0 on success, -ENODATA if not connected.
 *
 * @param exec_env  WAMR execution environment.
 * @param buf_ptr   WASM address of a ≥16-byte destination buffer.
 * @param len       Buffer capacity in bytes (minimum 16).
 * @return 0 on success; -ENODATA if no IP; -EINVAL/-EFAULT on bad args.
 */
int akira_native_net_get_ip(wasm_exec_env_t exec_env,
			    uint32_t buf_ptr, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_NET_API_H */
