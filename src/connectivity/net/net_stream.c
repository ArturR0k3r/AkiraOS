/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_net_stream
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_net_stream, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file net_stream.c
 * @brief TCP/UDP stream engine with shared-memory ring buffers.
 *
 * Architecture:
 *   - One poll thread monitors all active sockets via zsock_poll() (10 ms timeout).
 *   - On each iteration the poll thread drains all TX rings (WASM→host) and
 *     fills RX rings (host→WASM) from incoming data.
 *   - Network events (CONNECTED, DISCONNECTED, DATA_READY, ACCEPT, ERROR) are
 *     serialised into a k_msgq consumed by net_stream_event_pop().
 *   - Async connect/DNS uses the system workqueue (k_work) so the poll thread
 *     and the WASM app thread are never blocked on DNS resolution.
 *
 * Ring drain concurrency:
 *   - Only one entity may drain a TX ring at a time; guarded by an atomic flag.
 *   - Ring index updates are 32-bit aligned stores — atomic on all supported MCUs
 *     and within WASM's linear memory model.
 */

#ifdef CONFIG_AKIRA_WASM_NET

#include "net_stream.h"

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/atomic.h>
#include "mem_helper.h"

/* =========================================================================
 * Configuration defaults (overridden by Kconfig)
 * ======================================================================= */

#ifndef CONFIG_AKIRA_NET_MAX_STREAMS
#define CONFIG_AKIRA_NET_MAX_STREAMS      4
#endif

#ifndef CONFIG_AKIRA_NET_EVENT_QUEUE_DEPTH
#define CONFIG_AKIRA_NET_EVENT_QUEUE_DEPTH 8
#endif

#ifndef CONFIG_AKIRA_NET_POLL_STACK_SIZE
#define CONFIG_AKIRA_NET_POLL_STACK_SIZE  2048
#endif

#ifndef CONFIG_AKIRA_NET_MAX_MSG_SIZE
#define CONFIG_AKIRA_NET_MAX_MSG_SIZE     1500
#endif

/* =========================================================================
 * Internal stream state
 * ======================================================================= */

typedef enum {
	NET_STATE_CLOSED     = 0,
	NET_STATE_OPEN,        /* socket fd created, idle                      */
	NET_STATE_CONNECTING,  /* connect work queued or in progress           */
	NET_STATE_CONNECTED,   /* data can flow                                */
	NET_STATE_LISTENING,   /* TCP listen socket                            */
	NET_STATE_ERROR,       /* unrecoverable error — close and reopen       */
} net_state_t;

struct net_stream_ctx {
	bool         used;
	int          type;           /* NET_TYPE_TCP / NET_TYPE_UDP             */
	int          fd;             /* zsock fd, -1 if not yet open            */
	net_state_t  state;

	/* TX ring (WASM → host) */
	uint8_t     *tx_native;      /* native ptr into WASM linear memory      */
	uint32_t     tx_total;       /* total buffer size (hdr + data)          */
	atomic_t     tx_draining;   /* CAS flag: 1 = drain in progress          */

	/* RX ring (host → WASM) */
	uint8_t     *rx_native;      /* native ptr into WASM linear memory      */
	uint32_t     rx_total;       /* total buffer size (hdr + data)          */

	/* Async connect context */
	struct k_work connect_work;
	char          host[64];
	uint16_t      port;
};

/* =========================================================================
 * Global state
 * ======================================================================= */

static struct net_stream_ctx g_streams[CONFIG_AKIRA_NET_MAX_STREAMS];
static struct k_mutex g_mutex;
static bool g_initialized;

/* Event queue — mirrors the BLE g_evt_q pattern from ble_app_service.c */
K_MSGQ_DEFINE(g_net_evt_q,
	      sizeof(struct net_event),
	      CONFIG_AKIRA_NET_EVENT_QUEUE_DEPTH,
	      4);

/* Poll thread */
static void net_poll_thread_fn(void *a, void *b, void *c);
K_THREAD_STACK_DEFINE(g_poll_stack, CONFIG_AKIRA_NET_POLL_STACK_SIZE);
static struct k_thread g_poll_thread;

/* =========================================================================
 * Ring buffer helpers
 * ======================================================================= */

/**
 * @brief Drain the TX ring of @p ctx and send all pending messages.
 *
 * Entries are framed as: [len_lo][len_hi][payload...len bytes].
 * Uses an atomic CAS flag so only one caller drains at a time.
 *
 * @return Total payload bytes sent; 0 if ring empty; negative errno on error.
 */
static int ring_drain_tx(struct net_stream_ctx *ctx)
{
	if (!ctx->tx_native || ctx->fd < 0) {
		return 0;
	}

	/* Claim exclusive drain */
	if (!atomic_cas(&ctx->tx_draining, 0, 1)) {
		return 0; /* another thread is already draining */
	}

	struct net_ring_hdr *hdr = (struct net_ring_hdr *)ctx->tx_native;
	uint8_t *data = ctx->tx_native + NET_RING_HDR_SIZE;
	uint32_t cap = hdr->capacity;
	int total_sent = 0;

	if (cap == 0) {
		goto done;
	}

	while (true) {
		uint32_t wi = __atomic_load_n(&hdr->write_idx, __ATOMIC_ACQUIRE);
		uint32_t ri = hdr->read_idx; /* only we update read_idx */
		uint32_t used = wi - ri;

		if (used < 2) {
			break; /* not enough bytes for a 2-byte length header */
		}

		/* Peek at the 2-byte length header (may wrap) */
		uint32_t ri_mod = ri % cap;
		uint8_t  len_lo = data[ri_mod];
		uint8_t  len_hi = data[(ri_mod + 1) % cap];
		uint16_t plen   = (uint16_t)len_lo | ((uint16_t)len_hi << 8);

		if (used < (uint32_t)(2 + plen)) {
			break; /* incomplete message — wait for more writes */
		}

		if (plen == 0) {
			/* Skip zero-length sentinel */
			__atomic_store_n(&hdr->read_idx, ri + 2, __ATOMIC_RELEASE);
			continue;
		}

		if (plen > CONFIG_AKIRA_NET_MAX_MSG_SIZE) {
			LOG_WRN("TX ring: oversized message %u — dropping", plen);
			__atomic_store_n(&hdr->read_idx,
					 ri + 2 + plen, __ATOMIC_RELEASE);
			continue;
		}

		/* Copy payload from ring (handles wrap-around) */
		static AKIRA_BULK_BSS uint8_t payload[CONFIG_AKIRA_NET_MAX_MSG_SIZE];
		uint32_t data_ri = (ri_mod + 2) % cap;

		for (uint16_t i = 0; i < plen; i++) {
			payload[i] = data[(data_ri + i) % cap];
		}

		/* Send — WASM has set up the default peer via net_stream_connect */
		int ret = zsock_send(ctx->fd, payload, plen, 0);

		if (ret < 0) {
			int err = errno;
			LOG_ERR("zsock_send stream %td failed: %d",
				ctx - g_streams, err);
			break;
		}

		/* Advance consumer index (atomic store so WASM sees the update) */
		__atomic_store_n(&hdr->read_idx, ri + 2 + plen, __ATOMIC_RELEASE);
		total_sent += ret;
	}

done:
	atomic_set(&ctx->tx_draining, 0);
	return total_sent;
}

/**
 * @brief Write a received buffer into the RX ring of @p ctx.
 *
 * Entry format: [len_lo][len_hi][payload...].
 * The host is the sole producer of the RX ring; no CAS needed.
 *
 * @return @p in_len on success; -ENOBUFS if ring is full; -EINVAL if args bad.
 */
static int ring_fill_rx(struct net_stream_ctx *ctx,
			const uint8_t *in_data, uint16_t in_len)
{
	if (!ctx->rx_native || in_len == 0) {
		return -EINVAL;
	}

	struct net_ring_hdr *hdr = (struct net_ring_hdr *)ctx->rx_native;
	uint8_t *data = ctx->rx_native + NET_RING_HDR_SIZE;
	uint32_t cap  = hdr->capacity;

	if (cap == 0) {
		return -EINVAL;
	}

	uint32_t wi   = hdr->write_idx; /* host is sole writer */
	uint32_t ri   = __atomic_load_n(&hdr->read_idx, __ATOMIC_ACQUIRE);
	uint32_t used = wi - ri;
	uint32_t avail = cap - used;
	uint32_t need  = (uint32_t)(2 + in_len);

	if (avail < need) {
		return -ENOBUFS;
	}

	uint32_t wi_mod = wi % cap;

	data[wi_mod]             = in_len & 0xFFu;
	data[(wi_mod + 1) % cap] = (in_len >> 8) & 0xFFu;

	uint32_t data_wi = (wi_mod + 2) % cap;

	for (uint16_t i = 0; i < in_len; i++) {
		data[(data_wi + i) % cap] = in_data[i];
	}

	/* Atomic release so WASM consumer sees the complete write */
	__atomic_store_n(&hdr->write_idx, wi + need, __ATOMIC_RELEASE);
	return in_len;
}

/* =========================================================================
 * Async connect work
 * ======================================================================= */

static void connect_work_fn(struct k_work *work)
{
	struct net_stream_ctx *ctx =
		CONTAINER_OF(work, struct net_stream_ctx, connect_work);
	int h = (int)(ctx - g_streams);

	/* Resolve hostname */
	struct zsock_addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = (ctx->type == NET_TYPE_TCP)
				? SOCK_STREAM : SOCK_DGRAM,
	};
	struct zsock_addrinfo *res = NULL;
	int ret = zsock_getaddrinfo(ctx->host, NULL, &hints, &res);

	if (ret != 0 || !res) {
		LOG_ERR("stream %d: DNS failed for \"%s\": %d",
			h, ctx->host, ret);

		k_mutex_lock(&g_mutex, K_FOREVER);
		zsock_close(ctx->fd);
		ctx->fd    = -1;
		ctx->state = NET_STATE_ERROR;
		k_mutex_unlock(&g_mutex);

		struct net_event evt = {
			.type   = NET_EVT_ERROR,
			.handle = (uint8_t)h,
			.extra  = (uint16_t)EHOSTUNREACH,
		};
		k_msgq_put(&g_net_evt_q, &evt, K_NO_WAIT);
		return;
	}

	/* Patch in the target port */
	struct sockaddr_in *addr4 = (struct sockaddr_in *)res->ai_addr;
	addr4->sin_port = htons(ctx->port);

	ret = zsock_connect(ctx->fd,
			    (struct sockaddr *)addr4,
			    sizeof(*addr4));
	zsock_freeaddrinfo(res);

	k_mutex_lock(&g_mutex, K_FOREVER);

	if (ret < 0) {
		int err = errno;
		LOG_ERR("stream %d: connect to \"%s\":%u failed: %d",
			h, ctx->host, ctx->port, err);
		/* Close fd immediately so the socket slot is released */
		zsock_close(ctx->fd);
		ctx->fd    = -1;
		ctx->state = NET_STATE_ERROR;
		k_mutex_unlock(&g_mutex);

		struct net_event evt = {
			.type   = NET_EVT_ERROR,
			.handle = (uint8_t)h,
			.extra  = (uint16_t)err,
		};
		k_msgq_put(&g_net_evt_q, &evt, K_NO_WAIT);
		return;
	}

	ctx->state = NET_STATE_CONNECTED;
	k_mutex_unlock(&g_mutex);

	LOG_INF("stream %d: connected to \"%s\":%u", h, ctx->host, ctx->port);

	struct net_event evt = {
		.type   = NET_EVT_CONNECTED,
		.handle = (uint8_t)h,
		.extra  = 0,
	};
	k_msgq_put(&g_net_evt_q, &evt, K_NO_WAIT);
}

/* =========================================================================
 * Poll thread
 * ======================================================================= */

static void net_poll_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	LOG_INF("Net poll thread started");

	struct zsock_pollfd pfds[CONFIG_AKIRA_NET_MAX_STREAMS];
	int                 handles[CONFIG_AKIRA_NET_MAX_STREAMS];
	static AKIRA_BULK_BSS uint8_t rx_buf[CONFIG_AKIRA_NET_MAX_MSG_SIZE];

	while (1) {
		/* ---- 1. Snapshot active fds under mutex ---- */
		int nfds = 0;

		k_mutex_lock(&g_mutex, K_FOREVER);
		for (int i = 0; i < CONFIG_AKIRA_NET_MAX_STREAMS; i++) {
			struct net_stream_ctx *ctx = &g_streams[i];

			if (!ctx->used || ctx->fd < 0) {
				continue;
			}
			if (ctx->state == NET_STATE_CONNECTING ||
			    ctx->state == NET_STATE_CLOSED ||
			    ctx->state == NET_STATE_ERROR) {
				continue;
			}

			pfds[nfds].fd      = ctx->fd;
			pfds[nfds].events  = ZSOCK_POLLIN |
					     ZSOCK_POLLERR |
					     ZSOCK_POLLHUP;
			pfds[nfds].revents = 0;
			handles[nfds]      = i;
			nfds++;
		}
		k_mutex_unlock(&g_mutex);

		if (nfds == 0) {
			k_sleep(K_MSEC(10));
			continue;
		}

		/* ---- 2. Drain TX rings before polling ---- */
		for (int i = 0; i < nfds; i++) {
			int h = handles[i];
			struct net_stream_ctx *ctx = &g_streams[h];

			if (ctx->state == NET_STATE_CONNECTED && ctx->tx_native) {
				ring_drain_tx(ctx);
			}
		}

		/* ---- 3. Poll (10 ms timeout) ---- */
		int n = zsock_poll(pfds, nfds, 10);

		if (n <= 0) {
			continue;
		}

		/* ---- 4. Process events ---- */
		for (int i = 0; i < nfds; i++) {
			if (pfds[i].revents == 0) {
				continue;
			}

			int h = handles[i];
			struct net_stream_ctx *ctx = &g_streams[h];

			/* Error / hang-up */
			if (pfds[i].revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
				k_mutex_lock(&g_mutex, K_FOREVER);
				if (ctx->used) {
					ctx->state = NET_STATE_CLOSED;
				}
				k_mutex_unlock(&g_mutex);

				struct net_event evt = {
					.type   = NET_EVT_DISCONNECTED,
					.handle = (uint8_t)h,
					.extra  = 0,
				};
				k_msgq_put(&g_net_evt_q, &evt, K_NO_WAIT);
				continue;
			}

			if (!(pfds[i].revents & ZSOCK_POLLIN)) {
				continue;
			}

			/* Data or accept ready */
			k_mutex_lock(&g_mutex, K_FOREVER);
			net_state_t state = ctx->state;
			k_mutex_unlock(&g_mutex);

			if (state == NET_STATE_LISTENING) {
				/* Accept */
				int newfd = zsock_accept(ctx->fd, NULL, NULL);

				if (newfd < 0) {
					LOG_ERR("stream %d: accept failed: %d", h, errno);
					continue;
				}

				int new_h = -1;

				k_mutex_lock(&g_mutex, K_FOREVER);
				for (int j = 0; j < CONFIG_AKIRA_NET_MAX_STREAMS; j++) {
					if (!g_streams[j].used) {
						memset(&g_streams[j], 0, sizeof(g_streams[j]));
						g_streams[j].used  = true;
						g_streams[j].fd    = newfd;
						g_streams[j].type  = NET_TYPE_TCP;
						g_streams[j].state = NET_STATE_CONNECTED;
						k_work_init(&g_streams[j].connect_work,
							    connect_work_fn);
						new_h = j;
						break;
					}
				}
				k_mutex_unlock(&g_mutex);

				if (new_h >= 0) {
					LOG_INF("stream %d: accepted new conn → handle %d",
						h, new_h);
					struct net_event evt = {
						.type   = NET_EVT_ACCEPT,
						.handle = (uint8_t)h,
						.extra  = (uint16_t)new_h,
					};
					k_msgq_put(&g_net_evt_q, &evt, K_NO_WAIT);
				} else {
					LOG_WRN("stream %d: no free slots, closing accepted fd", h);
					zsock_close(newfd);
				}

			} else {
				/* Receive data */
				int recvd = zsock_recv(pfds[i].fd, rx_buf,
						       sizeof(rx_buf),
						       ZSOCK_MSG_DONTWAIT);

				if (recvd <= 0) {
					/* Peer closed or error */
					k_mutex_lock(&g_mutex, K_FOREVER);
					if (ctx->used) {
						ctx->state = NET_STATE_CLOSED;
					}
					k_mutex_unlock(&g_mutex);

					struct net_event evt = {
						.type   = NET_EVT_DISCONNECTED,
						.handle = (uint8_t)h,
						.extra  = 0,
					};
					k_msgq_put(&g_net_evt_q, &evt, K_NO_WAIT);
					continue;
				}

				/* Write into RX ring */
				k_mutex_lock(&g_mutex, K_FOREVER);
				uint8_t *rx_native = ctx->rx_native;
				k_mutex_unlock(&g_mutex);

				if (!rx_native) {
					LOG_DBG("stream %d: no RX ring bound, %d bytes dropped",
						h, recvd);
					continue;
				}

				int ret = ring_fill_rx(ctx, rx_buf, (uint16_t)recvd);

				if (ret < 0) {
					LOG_WRN("stream %d: RX ring full — %d bytes dropped",
						h, recvd);
					continue;
				}

				struct net_event evt = {
					.type   = NET_EVT_DATA_READY,
					.handle = (uint8_t)h,
					.extra  = 0,
				};
				k_msgq_put(&g_net_evt_q, &evt, K_NO_WAIT);
			}
		}
	}
}

/* =========================================================================
 * Public API
 * ======================================================================= */

int net_stream_init(void)
{
	if (g_initialized) {
		return 0;
	}

	k_mutex_init(&g_mutex);
	memset(g_streams, 0, sizeof(g_streams));
	k_msgq_purge(&g_net_evt_q);

	for (int i = 0; i < CONFIG_AKIRA_NET_MAX_STREAMS; i++) {
		g_streams[i].fd = -1;
		k_work_init(&g_streams[i].connect_work, connect_work_fn);
	}

	k_thread_create(&g_poll_thread, g_poll_stack,
			K_THREAD_STACK_SIZEOF(g_poll_stack),
			net_poll_thread_fn, NULL, NULL, NULL,
			CONFIG_AKIRA_WASM_APP_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&g_poll_thread, "akira_net_poll");

	g_initialized = true;
	LOG_INF("Net stream subsystem initialised (max_streams=%d)",
		CONFIG_AKIRA_NET_MAX_STREAMS);
	return 0;
}

int net_stream_open(int type)
{
	if (type != NET_TYPE_TCP && type != NET_TYPE_UDP) {
		return -EINVAL;
	}

	/*
	 * Lazy cleanup: reclaim any ERROR or CLOSED streams left over from a
	 * crashed WASM app that never called net_close().  This prevents fd
	 * exhaustion in the ESP32 LwIP socket pool and frees slots for reuse.
	 */
	k_mutex_lock(&g_mutex, K_FOREVER);
	for (int i = 0; i < CONFIG_AKIRA_NET_MAX_STREAMS; i++) {
		struct net_stream_ctx *c = &g_streams[i];

		if (c->used && (c->state == NET_STATE_ERROR ||
				c->state == NET_STATE_CLOSED)) {
			int leaked_fd = c->fd;

			memset(c, 0, sizeof(*c));
			c->fd = -1;
			k_work_init(&c->connect_work, connect_work_fn);

			if (leaked_fd >= 0) {
				zsock_close(leaked_fd);
			}
			LOG_INF("stream %d: reclaimed leaked slot", i);
		}
	}
	k_mutex_unlock(&g_mutex);

	int sock_type = (type == NET_TYPE_TCP) ? SOCK_STREAM : SOCK_DGRAM;
	int proto     = (type == NET_TYPE_TCP) ? IPPROTO_TCP : IPPROTO_UDP;

	int fd = zsock_socket(AF_INET, sock_type, proto);

	if (fd < 0) {
		LOG_ERR("zsock_socket(%s) failed: %d",
			(type == NET_TYPE_TCP) ? "TCP" : "UDP", errno);
		return -errno;
	}

	k_mutex_lock(&g_mutex, K_FOREVER);
	for (int i = 0; i < CONFIG_AKIRA_NET_MAX_STREAMS; i++) {
		if (!g_streams[i].used) {
			memset(&g_streams[i], 0, sizeof(g_streams[i]));
			g_streams[i].used  = true;
			g_streams[i].type  = type;
			g_streams[i].fd    = fd;
			g_streams[i].state = NET_STATE_OPEN;
			k_work_init(&g_streams[i].connect_work, connect_work_fn);
			k_mutex_unlock(&g_mutex);
			LOG_INF("stream %d opened (%s fd=%d)", i,
				(type == NET_TYPE_TCP) ? "TCP" : "UDP", fd);
			return i;
		}
	}
	k_mutex_unlock(&g_mutex);

	zsock_close(fd);
	LOG_ERR("No free stream slots (max=%d)", CONFIG_AKIRA_NET_MAX_STREAMS);
	return -ENOSPC;
}

int net_stream_connect(int handle, const char *host, uint16_t port)
{
	if (handle < 0 || handle >= CONFIG_AKIRA_NET_MAX_STREAMS || !host || !port) {
		return -EINVAL;
	}

	k_mutex_lock(&g_mutex, K_FOREVER);
	struct net_stream_ctx *ctx = &g_streams[handle];

	if (!ctx->used || ctx->fd < 0) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}
	if (ctx->state == NET_STATE_CONNECTING ||
	    ctx->state == NET_STATE_CONNECTED) {
		k_mutex_unlock(&g_mutex);
		return -EALREADY;
	}

	strncpy(ctx->host, host, sizeof(ctx->host) - 1);
	ctx->host[sizeof(ctx->host) - 1] = '\0';
	ctx->port  = port;
	ctx->state = NET_STATE_CONNECTING;
	k_mutex_unlock(&g_mutex);

	int ret = k_work_submit(&ctx->connect_work);

	if (ret < 0) {
		k_mutex_lock(&g_mutex, K_FOREVER);
		ctx->state = NET_STATE_OPEN;
		k_mutex_unlock(&g_mutex);
		LOG_ERR("stream %d: k_work_submit failed: %d", handle, ret);
		return -EAGAIN;
	}

	LOG_DBG("stream %d: connect to \"%s\":%u queued", handle, host, port);
	return 0;
}

int net_stream_bind(int handle, uint16_t port)
{
	if (handle < 0 || handle >= CONFIG_AKIRA_NET_MAX_STREAMS) {
		return -EINVAL;
	}

	k_mutex_lock(&g_mutex, K_FOREVER);
	struct net_stream_ctx *ctx = &g_streams[handle];

	if (!ctx->used || ctx->fd < 0) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}
	k_mutex_unlock(&g_mutex);

	int reuse = 1;
	zsock_setsockopt(ctx->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr   = { .s_addr = INADDR_ANY },
		.sin_port   = htons(port),
	};

	int ret = zsock_bind(ctx->fd, (struct sockaddr *)&addr, sizeof(addr));

	if (ret < 0) {
		LOG_ERR("stream %d: bind to port %u failed: %d", handle, port, errno);
		return -errno;
	}

	LOG_INF("stream %d: bound to port %u", handle, port);
	return 0;
}

int net_stream_listen(int handle, int backlog)
{
	if (handle < 0 || handle >= CONFIG_AKIRA_NET_MAX_STREAMS) {
		return -EINVAL;
	}

	k_mutex_lock(&g_mutex, K_FOREVER);
	struct net_stream_ctx *ctx = &g_streams[handle];

	if (!ctx->used || ctx->fd < 0) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}
	if (ctx->type != NET_TYPE_TCP) {
		k_mutex_unlock(&g_mutex);
		return -EPROTOTYPE;
	}

	int ret = zsock_listen(ctx->fd, backlog);

	if (ret < 0) {
		k_mutex_unlock(&g_mutex);
		LOG_ERR("stream %d: listen failed: %d", handle, errno);
		return -errno;
	}

	ctx->state = NET_STATE_LISTENING;
	k_mutex_unlock(&g_mutex);

	LOG_INF("stream %d: listening (backlog=%d)", handle, backlog);
	return 0;
}

int net_stream_close(int handle)
{
	if (handle < 0 || handle >= CONFIG_AKIRA_NET_MAX_STREAMS) {
		return -EINVAL;
	}

	k_mutex_lock(&g_mutex, K_FOREVER);
	struct net_stream_ctx *ctx = &g_streams[handle];

	if (!ctx->used) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}

	int fd = ctx->fd;

	/* Mark closed before releasing fd — poll thread sees state first */
	ctx->state      = NET_STATE_CLOSED;
	ctx->fd         = -1;
	ctx->tx_native  = NULL;
	ctx->rx_native  = NULL;
	ctx->used       = false;
	k_mutex_unlock(&g_mutex);

	if (fd >= 0) {
		zsock_close(fd);
	}

	LOG_INF("stream %d: closed", handle);
	return 0;
}

int net_stream_tx_bind(int handle, uint8_t *native_ptr, uint32_t total_size)
{
	if (handle < 0 || handle >= CONFIG_AKIRA_NET_MAX_STREAMS ||
	    !native_ptr || total_size <= NET_RING_HDR_SIZE) {
		return -EINVAL;
	}

	uint32_t cap = total_size - NET_RING_HDR_SIZE;

	k_mutex_lock(&g_mutex, K_FOREVER);
	struct net_stream_ctx *ctx = &g_streams[handle];

	if (!ctx->used) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}

	ctx->tx_native = native_ptr;
	ctx->tx_total  = total_size;
	k_mutex_unlock(&g_mutex);

	/* Initialise ring header: zero indices, set capacity */
	struct net_ring_hdr *hdr = (struct net_ring_hdr *)native_ptr;
	hdr->write_idx = 0;
	hdr->read_idx  = 0;
	hdr->capacity  = cap;
	hdr->flags     = 0;

	LOG_DBG("stream %d: TX ring bound (%u bytes data area)", handle, cap);
	return 0;
}

int net_stream_rx_bind(int handle, uint8_t *native_ptr, uint32_t total_size)
{
	if (handle < 0 || handle >= CONFIG_AKIRA_NET_MAX_STREAMS ||
	    !native_ptr || total_size <= NET_RING_HDR_SIZE) {
		return -EINVAL;
	}

	uint32_t cap = total_size - NET_RING_HDR_SIZE;

	k_mutex_lock(&g_mutex, K_FOREVER);
	struct net_stream_ctx *ctx = &g_streams[handle];

	if (!ctx->used) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}

	ctx->rx_native = native_ptr;
	ctx->rx_total  = total_size;
	k_mutex_unlock(&g_mutex);

	/* Initialise ring header */
	struct net_ring_hdr *hdr = (struct net_ring_hdr *)native_ptr;
	hdr->write_idx = 0;
	hdr->read_idx  = 0;
	hdr->capacity  = cap;
	hdr->flags     = 0;

	LOG_DBG("stream %d: RX ring bound (%u bytes data area)", handle, cap);
	return 0;
}

int net_stream_tx_flush(int handle)
{
	if (handle < 0 || handle >= CONFIG_AKIRA_NET_MAX_STREAMS) {
		return -EINVAL;
	}

	k_mutex_lock(&g_mutex, K_FOREVER);
	struct net_stream_ctx *ctx = &g_streams[handle];

	if (!ctx->used || ctx->state != NET_STATE_CONNECTED) {
		k_mutex_unlock(&g_mutex);
		return -ENOTCONN;
	}
	k_mutex_unlock(&g_mutex);

	return ring_drain_tx(ctx);
}

int net_stream_event_pop(struct net_event *evt)
{
	if (!evt) {
		return -EINVAL;
	}

	if (k_msgq_get(&g_net_evt_q, evt, K_NO_WAIT) == 0) {
		return evt->type;
	}

	return NET_EVT_NONE;
}

#else /* !CONFIG_AKIRA_WASM_NET */

/* Stubs — return -ENOTSUP when the module is disabled */
int net_stream_init(void)                                           { return -ENOTSUP; }
int net_stream_open(int t)                                          { (void)t; return -ENOTSUP; }
int net_stream_connect(int h, const char *host, uint16_t p)         { (void)h; (void)host; (void)p; return -ENOTSUP; }
int net_stream_bind(int h, uint16_t p)                              { (void)h; (void)p; return -ENOTSUP; }
int net_stream_listen(int h, int b)                                 { (void)h; (void)b; return -ENOTSUP; }
int net_stream_close(int h)                                         { (void)h; return -ENOTSUP; }
int net_stream_tx_bind(int h, uint8_t *p, uint32_t s)              { (void)h; (void)p; (void)s; return -ENOTSUP; }
int net_stream_rx_bind(int h, uint8_t *p, uint32_t s)              { (void)h; (void)p; (void)s; return -ENOTSUP; }
int net_stream_tx_flush(int h)                                      { (void)h; return -ENOTSUP; }
#include "net_stream.h"
int net_stream_event_pop(struct net_event *e)                       { (void)e; return -ENOTSUP; }

#endif /* CONFIG_AKIRA_WASM_NET */
