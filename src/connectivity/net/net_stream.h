/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_NET_STREAM_H
#define AKIRA_NET_STREAM_H

/**
 * @file net_stream.h
 * @brief Stream-based TCP/UDP/TLS network API for WASM apps and the system.
 *
 * Data path uses a **shared-memory ring buffer** living in the owner's memory
 * (WASM linear memory for apps):
 *   - TX ring: the owner writes packets, the poll thread drains them.
 *   - RX ring: the poll thread fills it from the socket, the owner reads it.
 *
 * Only net_tx_flush() / net_event_pop() require a WASM→host context switch.
 * Bulk data never does — it flows through the ring without any syscall per packet.
 *
 * Ring buffer layout (both TX and RX buffers share the same on-wire format):
 *
 *   Offset  0 (u32) : write_idx   — producer increments after writing
 *   Offset  4 (u32) : read_idx    — consumer increments after reading
 *   Offset  8 (u32) : capacity    — data area size, written by the host on bind
 *   Offset 12 (u32) : flags       — reserved (set to 0)
 *   Offset 16+      : data area   — variable-length framed messages:
 *                                   [ len_lo ][ len_hi ][ payload bytes... ]
 *
 * Indices are monotonically increasing; actual byte offset = idx % capacity.
 *
 * The header lives in memory the owner can write at any time, so the host
 * never trusts it: the capacity is fixed at bind time (total_size - 16) and
 * kept host-side, the host keeps its own copy of the index it produces/
 * consumes (TX read_idx, RX write_idx) and only publishes it to the header,
 * and every access is reduced modulo the stored capacity. A header that
 * claims more bytes than the ring can hold just stalls that ring.
 *
 * Ownership: every stream belongs to the owner that opened it (or that owns
 * the listening stream it was accepted on) — an app slot (0..N-1) or
 * NET_OWNER_SYSTEM (the console shell / app catalogue). Calls on a handle
 * another owner holds fail with -ENOENT, each owner has its own event queue,
 * and net_stream_release_owner() closes everything an app slot held when the
 * app ends.
 * @stability experimental
 * @since 1.4
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
/** @brief Socket type: TLS stream over TCP (requires CONFIG_AKIRA_WASM_NET_TLS) */
#define NET_TYPE_TLS  2

/** @brief Owner id of streams opened by the system (console shell, catalogue). */
#define NET_OWNER_SYSTEM  (-1)

/** @brief Size of the ring buffer header in bytes (must be kept in sync with WASM SDK) */
#define NET_RING_HDR_SIZE  16

/** @brief Longest hostname net_stream_connect() accepts (excluding the NUL). */
#define NET_STREAM_HOST_MAX  63

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
 * @brief Ring buffer header that lives at the start of each ring buffer.
 *
 * Documents the shared layout only: the host reads and writes these fields
 * byte-wise when the buffer is unaligned, and never trusts capacity.
 */
struct net_ring_hdr {
	uint32_t write_idx;  /**< Producer advances this                    */
	uint32_t read_idx;   /**< Consumer advances this                    */
	uint32_t capacity;   /**< Data area size (bytes); set once by host  */
	uint32_t flags;      /**< Reserved — must be 0                      */
} __packed;

/**
 * @brief Network event, as returned by net_stream_event_pop().
 *
 * Serialised into the WASM event buffer by the net_event_pop native:
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
 * Public API (called from akira_net_api.c and system code)
 *
 * @p owner is the calling app's slot (0..AKIRA_MAX_WASM_INSTANCES-1), or
 * NET_OWNER_SYSTEM for firmware code (console shell, app catalogue).
 *
 * None of these wait on the network: socket I/O and close are done by the
 * poll thread, DNS, connect and the TLS handshake by the connect worker on
 * the system workqueue. The only waits are bounded: a stream-table lock (-EBUSY if it
 * cannot be had within a second) and net_stream_close(), which waits up to
 * 200 ms for the socket to be closed.
 * ======================================================================= */

/**
 * @brief Initialise the network stream subsystem and start the poll thread.
 *
 * Idempotent and safe to call concurrently from any thread.
 *
 * @return 0 on success, negative errno on failure.
 */
int net_stream_init(void);

/**
 * @brief Open a new socket and allocate a stream slot owned by @p owner.
 *
 * When more than one app can run, one app may hold all slots but one. Apps
 * never get the last free slot: it is kept for NET_OWNER_SYSTEM.
 *
 * @param owner App slot, or NET_OWNER_SYSTEM.
 * @param type  NET_TYPE_TCP, NET_TYPE_UDP, or NET_TYPE_TLS.
 * @return Stream handle (>=0) on success; negative errno on failure.
 */
int net_stream_open(int owner, int type);

/**
 * @brief Queue an async connect to @p host : @p port.
 *
 * DNS and connect run on the system workqueue, one connect at a time
 * system-wide (NET_OWNER_SYSTEM's requests first, then request order). DNS is
 * bounded by CONFIG_NET_SOCKETS_DNS_TIMEOUT; the TCP connect is non-blocking
 * and re-checked every 50 ms for up to 10 s, so the workqueue (which also
 * feeds the watchdog) is never held for long. For TLS a non-blocking TCP
 * probe checks reachability first, then the handshake blocks the workqueue
 * for at most CONFIG_NET_SOCKETS_CONNECT_TIMEOUT. On completion
 * NET_EVT_CONNECTED or NET_EVT_ERROR is posted to the owner's event queue.
 *
 * For UDP, connect sets the default peer (no handshake).
 *
 * @param owner   Owner of @p handle.
 * @param handle  Stream handle from net_stream_open().
 * @param host    Null-terminated hostname or dotted-decimal IPv4 string,
 *                at most NET_STREAM_HOST_MAX characters.
 * @param port    Remote port (1–65535).
 * @return 0 if queued; -ENOENT if @p handle is not @p owner's; -EALREADY if
 *         already connecting/connected; -EINVAL if not connectable (listening,
 *         failed or closed); -ENAMETOOLONG if @p host is too long.
 */
int net_stream_connect(int owner, int handle, const char *host, uint16_t port);

/**
 * @brief Bind an unconnected socket to a local port.
 *
 * @param owner   Owner of @p handle.
 * @param handle  Stream handle.
 * @param port    Local port to bind (0 = let the stack choose).
 * @return 0 on success; negative errno on failure.
 */
int net_stream_bind(int owner, int handle, uint16_t port);

/**
 * @brief Mark a bound TCP socket as listening for inbound connections.
 *
 * On each accepted connection the poll thread allocates a new handle with the
 * same owner and posts NET_EVT_ACCEPT (extra = new handle).
 *
 * @param owner    Owner of @p handle.
 * @param handle   Stream handle (must be NET_TYPE_TCP).
 * @param backlog  Maximum length of the accept queue (clamped).
 * @return 0 on success; negative errno on failure.
 */
int net_stream_listen(int owner, int handle, int backlog);

/**
 * @brief Close a stream and free its slot.
 *
 * The rings are detached immediately (the host never touches them again).
 * The socket itself is closed by the poll thread; this call waits up to
 * 200 ms for that so a following open can reuse the socket. If a connect is
 * in flight it returns at once, and the socket is closed as soon as that
 * connect step returns.
 *
 * @param owner   Owner of @p handle.
 * @param handle  Stream handle.
 * @return 0 on success; -ENOENT if @p handle is not an open stream of @p owner.
 */
int net_stream_close(int owner, int handle);

/**
 * @brief Bind a TX ring buffer to a stream.
 *
 * The host writes the header (indices 0, capacity = @p total_size - 16) and
 * keeps the capacity and its read index privately from then on.
 *
 * @p native_ptr must stay valid and must not move until the stream is closed,
 * re-bound or released. The WASM native ensures that (it refuses to bind
 * rings in a linear memory that can still grow) and
 * net_stream_release_owner() detaches the rings before the instance is freed.
 *
 * @param owner       Owner of @p handle.
 * @param handle      Stream handle.
 * @param native_ptr  Native pointer to the start of the TX buffer (any alignment).
 * @param total_size  Total buffer size in bytes (must be > NET_RING_HDR_SIZE).
 * @return 0 on success; negative errno on failure.
 */
int net_stream_tx_bind(int owner, int handle, uint8_t *native_ptr, uint32_t total_size);

/**
 * @brief Bind an RX ring buffer to a stream. Same rules as net_stream_tx_bind().
 */
int net_stream_rx_bind(int owner, int handle, uint8_t *native_ptr, uint32_t total_size);

/**
 * @brief Ask the poll thread to drain the TX ring now.
 *
 * Sending is done by the poll thread only (non-blocking, partial sends
 * resumed when the socket is writable); it drains every ring on each of its
 * ≤10 ms iterations anyway, this just wakes it. Send errors arrive as
 * NET_EVT_ERROR.
 *
 * @param owner   Owner of @p handle.
 * @param handle  Stream handle.
 * @return 0 on success; -ENOTCONN if the stream is not connected; -ENOENT.
 */
int net_stream_tx_flush(int owner, int handle);

/**
 * @brief Pop the next event from @p owner's event queue (non-blocking).
 *
 * @param owner  App slot, or NET_OWNER_SYSTEM.
 * @param evt    Output: populated with the event data.
 * @return NET_EVT_* type (>0) if an event was available, NET_EVT_NONE if the
 *         queue is empty, -EINVAL on bad arguments.
 */
int net_stream_event_pop(int owner, struct net_event *evt);

/**
 * @brief Release everything app slot @p owner holds: close every stream it
 *        owns, stop all use of its rings and drop its queued events.
 *
 * When this returns the host no longer touches any memory the app bound as a
 * ring, so the WASM instance may be freed. Sockets are closed asynchronously
 * by the poll thread. Never blocks for long: safe from the stopping thread,
 * the system workqueue and the app's own thread. Idempotent; a no-op for a
 * slot that owns nothing.
 *
 * @param owner App slot; other values (including NET_OWNER_SYSTEM) are ignored.
 */
void net_stream_release_owner(int owner);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_NET_STREAM_H */
