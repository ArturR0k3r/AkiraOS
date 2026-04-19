/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_NET_STREAM_H
#define AKIRA_NET_STREAM_H

/**
 * @file net_stream.h
 * @brief Stream-based TCP/UDP network API for WASM apps.
 *
 * Data path uses a **shared-memory ring buffer** living in WASM linear memory:
 *   - TX ring: WASM writes packets, the poll thread drains them via zsock_send().
 *   - RX ring: the poll thread fills it from zsock_recv(), WASM reads from it.
 *
 * Only net_tx_flush() / net_event_pop() require a WASM→host context switch.
 * Bulk data never does — it flows through the ring without any syscall per packet.
 *
 * Ring buffer layout (both TX and RX buffers share the same on-wire format):
 *
 *   Offset  0 (u32) : write_idx   — producer increments after writing
 *   Offset  4 (u32) : read_idx    — consumer increments after reading
 *   Offset  8 (u32) : capacity    — data area size, set by host on bind; read-only after
 *   Offset 12 (u32) : flags       — reserved (set to 0)
 *   Offset 16+      : data area   — variable-length framed messages:
 *                                   [ len_lo ][ len_hi ][ payload bytes... ]
 *
 * Indices are monotonically increasing; actual byte offset = idx % capacity.
 * A 32-bit aligned read/write of write_idx / read_idx is atomically visible
 * on 32-bit MCUs (ESP32-S3, Cortex-M) and in WASM's linear memory model.
 */

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ======================================================================= */

/** @brief Socket type: TCP stream */
#define NET_TYPE_TCP  0
/** @brief Socket type: UDP datagram */
#define NET_TYPE_UDP  1

/** @brief Size of the ring buffer header in bytes (must be kept in sync with WASM SDK) */
#define NET_RING_HDR_SIZE  16

/** @brief Network event types returned by net_event_pop() */
#define NET_EVT_NONE         0
#define NET_EVT_CONNECTED    1  /**< TCP handshake complete (or UDP peer set)       */
#define NET_EVT_DISCONNECTED 2  /**< Peer closed connection or error occurred       */
#define NET_EVT_DATA_READY   3  /**< Data written to RX ring; start reading         */
#define NET_EVT_ACCEPT       4  /**< New inbound connection; extra = new handle     */
#define NET_EVT_ERROR        5  /**< Socket error; extra = positive errno           */

/* =========================================================================
 * Structures (internal — not exposed to WASM apps directly)
 * ======================================================================= */

/**
 * @brief Ring buffer header that lives at the start of each WASM buffer.
 *
 * Both the host (C) and the WASM app access this struct at the same memory
 * location (the native pointer obtained from wasm_runtime_addr_app_to_native
 * points at the same bytes as the WASM app's buffer pointer).
 */
struct net_ring_hdr {
	uint32_t write_idx;  /**< Producer advances this                    */
	uint32_t read_idx;   /**< Consumer advances this                    */
	uint32_t capacity;   /**< Data area size (bytes); set once by host  */
	uint32_t flags;      /**< Reserved — must be 0                      */
} __packed;

/**
 * @brief Network event placed into the k_msgq event queue.
 *
 * Serialised into the WASM event buffer by net_stream_event_pop():
 *   [0]   type   (uint8)
 *   [1]   handle (uint8)
 *   [2-3] extra  (uint16 LE)  — new handle for ACCEPT, errno for ERROR
 */
struct net_event {
	uint8_t  type;
	uint8_t  handle;
	uint16_t extra;
};

/* =========================================================================
 * Public API (called from akira_net_api.c)
 * ======================================================================= */

/**
 * @brief Initialise the network stream subsystem and start the poll thread.
 *
 * Called once during runtime init. Safe to call again (no-op if already up).
 *
 * @return 0 on success, negative errno on failure.
 */
int net_stream_init(void);

/**
 * @brief Open a new socket and allocate a stream slot.
 *
 * @param type  NET_TYPE_TCP or NET_TYPE_UDP.
 * @return Stream handle (>=0) on success; negative errno on failure.
 */
int net_stream_open(int type);

/**
 * @brief Initiate an async connect to @p host : @p port.
 *
 * DNS resolution and connect() are dispatched to the system workqueue so
 * they do not block the calling WASM thread or the poll thread.
 * On completion, NET_EVT_CONNECTED or NET_EVT_ERROR is posted.
 *
 * For UDP, zsock_connect() sets the default peer (no three-way handshake).
 *
 * @param handle  Stream handle from net_stream_open().
 * @param host    Null-terminated hostname or dotted-decimal IPv4 string.
 * @param port    Remote port (1–65535).
 * @return 0 if work was queued; negative errno on immediate error.
 */
int net_stream_connect(int handle, const char *host, uint16_t port);

/**
 * @brief Bind a socket to a local port.
 *
 * Required before net_stream_listen() for TCP servers, or for UDP servers
 * that need to receive datagrams on a specific port.
 *
 * @param handle  Stream handle.
 * @param port    Local port to bind (0 = let kernel choose).
 * @return 0 on success; negative errno on failure.
 */
int net_stream_bind(int handle, uint16_t port);

/**
 * @brief Mark a TCP socket as listening for inbound connections.
 *
 * Must be called after net_stream_bind(). On each accepted connection the
 * poll thread allocates a new handle and posts NET_EVT_ACCEPT.
 *
 * @param handle   Stream handle (must be NET_TYPE_TCP).
 * @param backlog  Maximum length of the accept queue.
 * @return 0 on success; negative errno on failure.
 */
int net_stream_listen(int handle, int backlog);

/**
 * @brief Close a stream socket and free its slot.
 *
 * Any pending TX / RX ring data is discarded. The poll thread detects the
 * closure on its next iteration (POLLNVAL) and removes the fd from its set.
 *
 * @param handle  Stream handle.
 * @return 0 on success; negative errno on failure.
 */
int net_stream_close(int handle);

/**
 * @brief Bind a TX ring buffer (living in WASM linear memory) to a stream.
 *
 * @p native_ptr must be the native C pointer for the WASM buffer, obtained via
 * wasm_runtime_addr_app_to_native(). The first NET_RING_HDR_SIZE bytes are the
 * ring header; the host writes @p total_size - NET_RING_HDR_SIZE into hdr->capacity.
 *
 * @param handle      Stream handle.
 * @param native_ptr  Native pointer to the start of the WASM TX buffer.
 * @param total_size  Total buffer size in bytes (must be > NET_RING_HDR_SIZE).
 * @return 0 on success; negative errno on failure.
 */
int net_stream_tx_bind(int handle, uint8_t *native_ptr, uint32_t total_size);

/**
 * @brief Bind an RX ring buffer (living in WASM linear memory) to a stream.
 *
 * @param handle      Stream handle.
 * @param native_ptr  Native pointer to the start of the WASM RX buffer.
 * @param total_size  Total buffer size in bytes (must be > NET_RING_HDR_SIZE).
 * @return 0 on success; negative errno on failure.
 */
int net_stream_rx_bind(int handle, uint8_t *native_ptr, uint32_t total_size);

/**
 * @brief Drain the TX ring and send all pending messages immediately.
 *
 * The poll thread drains the TX ring on each of its 10 ms iterations.
 * Call this function to force an immediate send without waiting.
 *
 * @param handle  Stream handle.
 * @return Total bytes sent on success; negative errno on failure.
 */
int net_stream_tx_flush(int handle);

/**
 * @brief Pop the next network event from the event queue (non-blocking).
 *
 * @param evt  Output: populated with the event data.
 * @return NET_EVT_* type (>0) if an event was available, 0 if the queue is empty.
 */
int net_stream_event_pop(struct net_event *evt);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_NET_STREAM_H */
