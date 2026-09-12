/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef CATALOG_NET_H
#define CATALOG_NET_H

#include <stdint.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CATALOG_NET_RING_HDR_SIZE 16 /* must equal NET_RING_HDR_SIZE in net_stream.h */

/** Initialize a ring buffer's header in a caller-owned buffer.
 *  @param buf        Buffer of at least CATALOG_NET_RING_HDR_SIZE + 1 bytes.
 *  @param total_size Total buffer size in bytes.
 */
void catalog_ring_init(uint8_t *buf, uint32_t total_size);

/** Write one framed message into the ring, advancing write_idx.
 *  @return 0 on success, -ENOSPC if the message doesn't fit in free space.
 */
int catalog_ring_write(uint8_t *buf, const uint8_t *msg, uint16_t msg_len);

/** Read the next framed message out of the ring, advancing read_idx.
 *  @param out      Caller-owned buffer to copy the payload into.
 *  @param out_size Size of @p out.
 *  @return Number of bytes copied into @p out (>=0), or -ENODATA if the ring
 *          is empty, or -ENOBUFS if the next message is larger than @p out_size.
 */
int catalog_ring_read(uint8_t *buf, uint8_t *out, uint32_t out_size);

/** Connect to host:port over TLS, send an HTTP/1.1 GET for @p path, and
 *  collect the full response body (skips HTTP headers) into @p out.
 *  Blocks the calling thread — do not call from a time-critical context.
 *  @param timeout_ms  Overall timeout for connect + full response.
 *  @return Number of body bytes written to @p out (>=0), or negative errno
 *          (-ETIMEDOUT, -ECONNREFUSED, or whatever net_stream_event_pop
 *          reported via NET_EVT_ERROR's extra field).
 */
int catalog_https_get(const char *host, uint16_t port, const char *path,
                       uint8_t *out, uint32_t out_size, int timeout_ms);

/** Open a TLS connection to host:port and leave it open for reuse across
 *  multiple catalog_https_get_on() calls, instead of paying a fresh TLS
 *  handshake (routinely ~15s against console.app.akiraos.dev on this
 *  hardware) per request. Blocks until connected or @p timeout_ms elapses.
 *  @return Stream handle (>=0) for catalog_https_get_on()/close(), or
 *          negative errno.
 */
int catalog_https_open(const char *host, uint16_t port, int timeout_ms);

/** Perform one GET on a connection from catalog_https_open(), keeping it
 *  open afterwards (Connection: keep-alive) — completion is detected via
 *  the response's Content-Length rather than waiting for the peer to close.
 *  Falls back to "wait for disconnect" only if Content-Length is absent.
 *  Does not follow redirects (unlike catalog_https_get()).
 *  @return Number of body bytes written to @p out (>=0), or negative errno.
 */
int catalog_https_get_on(int handle, const char *host, const char *path,
                         uint8_t *out, uint32_t out_size, int timeout_ms);

/** Close a connection opened by catalog_https_open(). */
void catalog_https_close(int handle);

#ifdef __cplusplus
}
#endif

#endif /* CATALOG_NET_H */
