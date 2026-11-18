/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_net_stream
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_net_stream, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file net_stream.c
 * @brief TCP/UDP/TLS stream engine with shared-memory ring buffers.
 *
 * Threads and who may do what:
 *   - Poll thread: the only thread that sends, receives, accepts, touches a
 *     ring, or closes a socket that is not being connected. It drains TX
 *     rings and fills RX rings with non-blocking socket calls, so one stalled
 *     peer never holds up the others.
 *   - Connect worker (system workqueue, one k_work_delayable): one connect at
 *     a time system-wide (the system owner's requests first). DNS (bounded by
 *     CONFIG_NET_SOCKETS_DNS_TIMEOUT) and the TLS handshake (bounded by
 *     CONFIG_NET_SOCKETS_CONNECT_TIMEOUT) are the only steps that block it;
 *     a TCP connect (and the TLS reachability probe) is started non-blocking
 *     and re-checked every 50 ms, so the workqueue — which also feeds the
 *     watchdog and runs app cleanup — keeps running in between. It never
 *     waits for the stream table either: if the lock is not free it tries
 *     again later. While it connects a stream it owns that socket.
 *   - Callers (app natives, and the system owner: console shell/catalogue):
 *     validate ownership and flip state under g_mutex, never wait on the
 *     network. Their only socket calls are socket()/bind()/listen(), which
 *     do not block.
 *
 * Memory safety against the app: ring capacity is fixed at bind time, the
 * host keeps its own copy of the index it produces/consumes, every offset is
 * reduced modulo that capacity, and the poll thread accesses ring memory only
 * while holding g_mutex with the pointer re-checked — so detaching a ring
 * (close / net_stream_release_owner) under g_mutex ends all access to it.
 *
 * Slot lifetime: close/release only mark a stream "closing" and detach its
 * rings; the poll thread closes the socket — once the connect worker has let
 * go of it, if it was connecting — and only then is the slot freed and its
 * generation bumped. Because only the poll thread closes stream sockets, an
 * fd number is never reused (by another stream, or another subsystem's
 * socket) while the poll thread may still use it.
 */

#ifdef CONFIG_AKIRA_WASM_NET

#include "net_stream.h"

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/sys/util.h>
#include "mem_helper.h"
#include <runtime/akira_runtime.h> /* AKIRA_MAX_WASM_INSTANCES */

#ifdef CONFIG_AKIRA_WDT
#include <drivers/wdt/akira_wdt.h>
#endif

#ifdef CONFIG_AKIRA_WASM_NET_TLS
#include <zephyr/net/tls_credentials.h>
#include "ca_certificate.h"

#define NET_TLS_CA_SEC_TAG_GTS_R4   1
#define NET_TLS_CA_SEC_TAG_ISRG_X2  2
#define NET_TLS_CA_SEC_TAG_GTS_R1   3
#endif

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

#define NET_MAX_STREAMS     CONFIG_AKIRA_NET_MAX_STREAMS
#define NET_EVT_DEPTH       CONFIG_AKIRA_NET_EVENT_QUEUE_DEPTH
/* Event rings: one per app slot plus one for NET_OWNER_SYSTEM (index 0). */
#define NET_OWNER_COUNT     (AKIRA_MAX_WASM_INSTANCES + 1)

/* The last free slot is kept for the system owner (catalogue, shell), so
 * apps can never lock the console out of the network. */
#define NET_SYSTEM_RESERVE  ((NET_MAX_STREAMS > 1) ? 1 : 0)

/* With more than one app slot, one app can never take every stream: one is
 * always left for another app. */
#if AKIRA_MAX_WASM_INSTANCES > 1
#define NET_STREAMS_PER_APP MAX(1, NET_MAX_STREAMS - 1)
#else
#define NET_STREAMS_PER_APP NET_MAX_STREAMS
#endif

/* zsock_poll() takes at most CONFIG_ZVFS_POLL_MAX wait events (POLLIN, and
 * POLLOUT on TCP, use one each); more and it fails with ENOMEM. */
#ifdef CONFIG_ZVFS_POLL_MAX
#define NET_POLL_EVENTS_MAX CONFIG_ZVFS_POLL_MAX
#else
#define NET_POLL_EVENTS_MAX (2 * NET_MAX_STREAMS)
#endif

#define NET_POLL_TICK_MS        10    /* poll thread iteration                */
#define NET_CONNECT_POLL_MS     50    /* connect worker re-check interval     */
#define NET_TCP_CONNECT_MS      10000 /* plain TCP connect deadline           */
#define NET_TLS_PROBE_MS        6000  /* TLS: TCP reachability probe deadline */
#define NET_CLOSE_WAIT_MS       200   /* close() waits this long for the reap */
#define NET_TX_BUDGET           8     /* messages per stream per iteration    */
#define NET_CALLER_LOCK_MS      1000  /* natives: -EBUSY after this           */
#define NET_WORKER_LOCK_MS      100   /* connect worker: retry later after    */
#define NET_RELEASE_LOCK_MS     2000  /* release hook: give up after          */
#define NET_LOG_INTERVAL_MS     1000  /* app-triggered warnings: one per      */

#define RING_OFF_WRITE  0
#define RING_OFF_READ   4
#define RING_OFF_CAP    8
#define RING_OFF_FLAGS  12

BUILD_ASSERT(NET_MAX_STREAMS <= 255, "stream handles are reported as uint8");
BUILD_ASSERT(NET_EVT_DEPTH <= 255, "event ring indices are uint8");
BUILD_ASSERT(CONFIG_AKIRA_NET_MAX_MSG_SIZE <= 0xFFFF, "ring frame length is u16");
BUILD_ASSERT(NET_POLL_EVENTS_MAX >= 2, "one stream may need two poll events");
BUILD_ASSERT(AKIRA_MAX_WASM_INSTANCES <= 127, "owner is stored as int8");
#ifdef CONFIG_NET_SOCKETS_DNS_TIMEOUT
/* DNS is the one step that blocks the connect worker, on the system
 * workqueue that also feeds the watchdog. */
BUILD_ASSERT(CONFIG_NET_SOCKETS_DNS_TIMEOUT <= 10000,
	     "DNS lookups must stay far below the watchdog timeout");
#endif

/* =========================================================================
 * Internal stream state
 * ======================================================================= */

typedef enum {
	NET_STATE_CLOSED     = 0,  /* peer closed / poll error; fd kept until close */
	NET_STATE_OPEN,            /* socket created, idle                          */
	NET_STATE_CONNECTING,      /* queued for / owned by the connect worker      */
	NET_STATE_CONNECTED,       /* data can flow                                 */
	NET_STATE_LISTENING,       /* TCP listen socket                             */
	NET_STATE_ERROR,           /* connect/send failed                           */
} net_state_t;

struct net_stream_ctx {
	bool         used;
	bool         closing;      /* close requested: reaper closes fd, frees slot  */
	bool         conn_queued;  /* waiting for the connect worker                 */
	bool         conn_active;  /* the connect worker owns fd right now           */
	bool         tx_wait_out;  /* send hit EAGAIN / was partial: wait POLLOUT    */
	bool         drop_fd;      /* connect failed: reaper closes fd, keeps slot   */
	int8_t       owner;        /* app slot, or NET_OWNER_SYSTEM                  */
	uint8_t      type;         /* NET_TYPE_*                                     */
	net_state_t  state;
	int          fd;           /* zsock fd, -1 if none                           */
	uint32_t     gen;          /* bumped every time the slot is freed            */
	uint32_t     conn_seq;     /* connect request order                          */

	/* TX ring (app → network). Only cap and tx_ri are trusted. */
	uint8_t     *tx_native;
	uint32_t     tx_cap;
	uint32_t     tx_epoch;     /* bumped on every (re)bind                       */
	uint32_t     tx_ri;        /* host consumer index (authoritative)            */
	uint32_t     tx_sent;      /* bytes of the current message already sent      */
	uint16_t     tx_cur_len;   /* length of the message being sent               */

	/* RX ring (network → app). Only cap and rx_wi are trusted. */
	uint8_t     *rx_native;
	uint32_t     rx_cap;
	uint32_t     rx_wi;        /* host producer index (authoritative)            */

	char         host[NET_STREAM_HOST_MAX + 1];
	uint16_t     port;
};

/* Per-owner event ring — one app can neither read nor crowd out another's. */
struct net_evt_ring {
	uint8_t          head;
	uint8_t          count;
	struct net_event ev[NET_EVT_DEPTH];
};

/* =========================================================================
 * Global state
 *
 * Kernel objects (mutexes, semaphore, delayable work, thread) stay in
 * internal RAM: the scheduler and the timer ISR walk them. Plain data goes to
 * PSRAM where there is some.
 * ======================================================================= */

static struct net_stream_ctx AKIRA_BULK_BSS g_streams[NET_MAX_STREAMS];
static struct net_evt_ring AKIRA_BULK_BSS g_evt[NET_OWNER_COUNT];
static struct k_spinlock g_evt_lock;

static K_MUTEX_DEFINE(g_mutex);     /* stream table + ring memory access */
static K_MUTEX_DEFINE(g_init_lock);
static bool g_initialized;
static uint32_t g_conn_seq;

/* Poll thread */
static void net_poll_thread_fn(void *a, void *b, void *c);
K_THREAD_STACK_DEFINE(g_poll_stack, CONFIG_AKIRA_NET_POLL_STACK_SIZE);
static struct k_thread g_poll_thread;

/* Poll-thread-only buffers (never shared with another thread). */
static AKIRA_BULK_BSS uint8_t g_rx_buf[CONFIG_AKIRA_NET_MAX_MSG_SIZE];
static AKIRA_BULK_BSS uint8_t g_tx_stage[CONFIG_AKIRA_NET_MAX_MSG_SIZE];

/* Wakes the poll thread: a stream became pollable, something must be reaped,
 * or a flush was requested. A give before the take is remembered. */
static K_SEM_DEFINE(g_poll_wake, 0, 1);

/* Connect worker: the single connect in flight. Only the worker touches g_job. */
enum {
	CP_START = 0,     /* resolve + start connecting                      */
	CP_TCP_WAIT,      /* non-blocking TCP connect in progress on job.fd  */
	CP_PROBE_WAIT,    /* TLS: non-blocking probe connect on job.probe_fd */
	CP_FINISH,        /* done; the outcome still has to be recorded      */
};

static struct {
	bool               active;
	uint8_t            phase;
	uint8_t            type;
	int                h;
	uint32_t           gen;
	int                fd;
	int                probe_fd;   /* TLS reachability probe, worker-owned */
	int                result;
	uint16_t           port;
	int64_t            deadline;
	struct sockaddr_in addr;
	char               host[NET_STREAM_HOST_MAX + 1];
} g_job AKIRA_BULK_BSS;

static void connect_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_conn_work, connect_work_fn);

/* =========================================================================
 * Small helpers
 * ======================================================================= */

/* App-triggered warnings: at most one per NET_LOG_INTERVAL_MS, so an app in a
 * loop cannot flood the log. */
static bool net_log_allowed(void)
{
	static uint32_t last;
	uint32_t now = k_uptime_get_32();

	if (last != 0U && (now - last) < NET_LOG_INTERVAL_MS) {
		return false;
	}
	last = now ? now : 1U;
	return true;
}

#define NET_WRN(...)                                                           \
	do {                                                                   \
		if (net_log_allowed()) {                                       \
			LOG_WRN(__VA_ARGS__);                                  \
		}                                                              \
	} while (0)

static inline bool owner_valid(int owner)
{
	return owner >= NET_OWNER_SYSTEM && owner < AKIRA_MAX_WASM_INSTANCES;
}

/* Event ring of a valid @p owner (NET_OWNER_SYSTEM is index 0). */
static inline struct net_evt_ring *owner_ring(int owner)
{
	return &g_evt[owner + 1];
}

static inline int ctx_handle(const struct net_stream_ctx *c)
{
	return (int)(c - g_streams);
}

/* Slot still the one we looked at, and not being torn down. g_mutex held. */
static inline bool ctx_live(const struct net_stream_ctx *c, uint32_t gen)
{
	return c->used && !c->closing && c->gen == gen;
}

/* Handle @p h, if it is an open stream of @p owner. g_mutex held. */
static struct net_stream_ctx *owned_locked(int owner, int h)
{
	if (h < 0 || h >= NET_MAX_STREAMS) {
		return NULL;
	}

	struct net_stream_ctx *c = &g_streams[h];

	if (!c->used || c->closing || c->owner != owner) {
		return NULL;
	}
	return c;
}

/* Natives never wait long for the table: a holder that died with it would
 * otherwise hang every app that touches the network. */
static int caller_lock(void)
{
	return (k_mutex_lock(&g_mutex, K_MSEC(NET_CALLER_LOCK_MS)) == 0) ? 0 : -EBUSY;
}

static int owner_count_locked(int owner)
{
	int n = 0;

	for (int i = 0; i < NET_MAX_STREAMS; i++) {
		if (g_streams[i].used && g_streams[i].owner == owner) {
			n++;
		}
	}
	return n;
}

static int free_count_locked(void)
{
	int n = 0;

	for (int i = 0; i < NET_MAX_STREAMS; i++) {
		if (!g_streams[i].used) {
			n++;
		}
	}
	return n;
}

/* Apps are held to NET_STREAMS_PER_APP and never get the last free slot
 * (NET_SYSTEM_RESERVE); the system owner may use any free slot. */
static bool owner_at_cap_locked(int owner)
{
	if (owner == NET_OWNER_SYSTEM) {
		return false;
	}
	return owner_count_locked(owner) >= NET_STREAMS_PER_APP ||
	       free_count_locked() <= NET_SYSTEM_RESERVE;
}

static int slot_alloc_locked(int owner, int type, int fd)
{
	if (owner_at_cap_locked(owner)) {
		return -ENOSPC;
	}

	for (int i = 0; i < NET_MAX_STREAMS; i++) {
		struct net_stream_ctx *c = &g_streams[i];

		if (c->used) {
			continue;
		}

		uint32_t gen = c->gen;

		memset(c, 0, sizeof(*c));
		c->gen   = gen;
		c->used  = true;
		c->owner = (int8_t)owner;
		c->type  = (uint8_t)type;
		c->fd    = fd;
		c->state = NET_STATE_OPEN;
		return i;
	}
	return -ENOSPC;
}

/* Free a slot whose socket is already closed. g_mutex held. */
static void slot_free_locked(struct net_stream_ctx *c)
{
	uint32_t gen = c->gen + 1;

	memset(c, 0, sizeof(*c));
	c->gen = gen;
	c->fd  = -1;
}

static int set_nonblock(int fd)
{
	int fl = zsock_fcntl(fd, ZVFS_F_GETFL, 0);

	if (fl < 0) {
		return -errno;
	}
	if (zsock_fcntl(fd, ZVFS_F_SETFL, fl | ZVFS_O_NONBLOCK) < 0) {
		return -errno;
	}
	return 0;
}

/* =========================================================================
 * Events (per owner)
 * ======================================================================= */

/* Post to @p c's owner. Called with g_mutex held so it is ordered against
 * net_stream_release_owner()'s purge: a released owner never gets a late
 * event that a new app in the same slot would then read. */
static void evt_post_locked(const struct net_stream_ctx *c, uint8_t type, uint16_t extra)
{
	if (!owner_valid(c->owner)) {
		return;
	}

	struct net_evt_ring *r = owner_ring(c->owner);
	bool dropped = false;
	k_spinlock_key_t key = k_spin_lock(&g_evt_lock);

	if (r->count < NET_EVT_DEPTH) {
		struct net_event *e = &r->ev[(r->head + r->count) % NET_EVT_DEPTH];

		e->type   = type;
		e->handle = (uint8_t)ctx_handle(c);
		e->extra  = extra;
		r->count++;
	} else {
		dropped = true;
	}
	k_spin_unlock(&g_evt_lock, key);

	if (dropped) {
		NET_WRN("stream %d: owner %d event queue full, event %u dropped",
			ctx_handle(c), c->owner, type);
	}
}

static int evt_pop(int owner, struct net_event *out)
{
	struct net_evt_ring *r = owner_ring(owner);
	int type = NET_EVT_NONE;
	k_spinlock_key_t key = k_spin_lock(&g_evt_lock);

	if (r->count > 0) {
		*out = r->ev[r->head];
		r->head = (uint8_t)((r->head + 1) % NET_EVT_DEPTH);
		r->count--;
		type = out->type;
	}
	k_spin_unlock(&g_evt_lock, key);
	return type;
}

/* Drop @p owner's queued events for handle @p h (closed: the number may be
 * reused). Called with g_mutex held, like every post. */
static void evt_purge_handle(int owner, int h)
{
	struct net_evt_ring *r = owner_ring(owner);
	k_spinlock_key_t key = k_spin_lock(&g_evt_lock);
	uint8_t kept = 0;

	for (uint8_t i = 0; i < r->count; i++) {
		struct net_event e = r->ev[(r->head + i) % NET_EVT_DEPTH];

		if (e.handle != (uint8_t)h) {
			r->ev[(r->head + kept) % NET_EVT_DEPTH] = e;
			kept++;
		}
	}
	r->count = kept;
	k_spin_unlock(&g_evt_lock, key);
}

static void evt_purge(int owner)
{
	struct net_evt_ring *r = owner_ring(owner);
	k_spinlock_key_t key = k_spin_lock(&g_evt_lock);

	r->head  = 0;
	r->count = 0;
	k_spin_unlock(&g_evt_lock, key);
}

/* =========================================================================
 * Ring access
 *
 * Rings live in owner memory (WASM linear memory for apps): any alignment
 * (byte-wise access when unaligned — an unaligned 32-bit load faults on
 * Xtensa), any content. Callers hold g_mutex and have checked the ring
 * pointer.
 * ======================================================================= */

static uint32_t ring_ld32(const uint8_t *p)
{
	uint32_t v;

	if (((uintptr_t)p & 3U) == 0U) {
		v = __atomic_load_n((const uint32_t *)(const void *)p, __ATOMIC_ACQUIRE);
	} else {
		const volatile uint8_t *b = p;

		v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
		    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
	}
	return v;
}

static void ring_st32(uint8_t *p, uint32_t v)
{
	if (((uintptr_t)p & 3U) == 0U) {
		__atomic_store_n((uint32_t *)(void *)p, v, __ATOMIC_RELEASE);
	} else {
		volatile uint8_t *b = p;

		__atomic_thread_fence(__ATOMIC_RELEASE);
		b[0] = (uint8_t)v;
		b[1] = (uint8_t)(v >> 8);
		b[2] = (uint8_t)(v >> 16);
		b[3] = (uint8_t)(v >> 24);
	}
}

static void ring_hdr_init(uint8_t *ring, uint32_t cap)
{
	ring_st32(ring + RING_OFF_WRITE, 0);
	ring_st32(ring + RING_OFF_READ, 0);
	ring_st32(ring + RING_OFF_CAP, cap);
	ring_st32(ring + RING_OFF_FLAGS, 0);
}

/* Data-area offset of byte @p skip past index @p idx (matches the SDK's
 * (idx % cap + skip) % cap without 32-bit overflow). */
static inline uint32_t ring_off(uint32_t idx, uint32_t skip, uint32_t cap)
{
	return (uint32_t)(((uint64_t)(idx % cap) + skip) % cap);
}

/* @p off < @p cap; @p len is clamped to @p cap, so both copies stay inside
 * the data area. */
static void ring_copy_out(uint8_t *dst, const uint8_t *data, uint32_t cap,
			  uint32_t off, uint32_t len)
{
	len = MIN(len, cap);

	uint32_t first = MIN(len, cap - off);

	memcpy(dst, data + off, first);
	memcpy(dst + first, data, len - first);
}

static void ring_copy_in(uint8_t *data, uint32_t cap, uint32_t off,
			 const uint8_t *src, uint32_t len)
{
	len = MIN(len, cap);

	uint32_t first = MIN(len, cap - off);

	memcpy(data + off, src, first);
	memcpy(data, src + first, len - first);
}

static void tx_consume_locked(struct net_stream_ctx *c, uint32_t n)
{
	c->tx_ri += n;
	ring_st32(c->tx_native + RING_OFF_READ, c->tx_ri);
}

/* Free bytes in @p c's RX ring; 0 if the app's read_idx is nonsense (a
 * corrupt header stalls that ring, it never widens it). */
static uint32_t rx_room_locked(const struct net_stream_ctx *c)
{
	uint32_t ri   = ring_ld32(c->rx_native + RING_OFF_READ);
	uint32_t used = c->rx_wi - ri;

	if (used > c->rx_cap) {
		return 0;
	}
	return c->rx_cap - used;
}

static int rx_push_locked(struct net_stream_ctx *c, const uint8_t *buf, uint16_t len)
{
	uint32_t need = 2U + len;

	if (rx_room_locked(c) < need) {
		return -ENOBUFS;
	}

	uint8_t *data = c->rx_native + NET_RING_HDR_SIZE;
	uint32_t cap  = c->rx_cap;
	uint8_t  hdr[2] = { (uint8_t)(len & 0xFFu), (uint8_t)(len >> 8) };

	ring_copy_in(data, cap, ring_off(c->rx_wi, 0, cap), hdr, 2);
	ring_copy_in(data, cap, ring_off(c->rx_wi, 2, cap), buf, len);

	c->rx_wi += need;
	ring_st32(c->rx_native + RING_OFF_WRITE, c->rx_wi); /* release: data first */
	return len;
}

/* =========================================================================
 * Poll thread: TX drain, RX fill, accept, reap
 * ======================================================================= */

/**
 * Send what @p h's TX ring holds, non-blocking. The message is copied out
 * under g_mutex, sent without it, and only consumed once fully sent; a
 * partial send or EAGAIN parks the stream until POLLOUT.
 *
 * @return true if the budget ran out with more messages queued.
 */
static bool tx_drain(int h)
{
	struct net_stream_ctx *c = &g_streams[h];

	for (int n = 0; n < NET_TX_BUDGET; n++) {
		k_mutex_lock(&g_mutex, K_FOREVER);

		if (!c->used || c->closing || c->state != NET_STATE_CONNECTED ||
		    !c->tx_native || c->fd < 0 || c->tx_wait_out) {
			k_mutex_unlock(&g_mutex);
			return false;
		}

		uint32_t gen   = c->gen;
		uint32_t epoch = c->tx_epoch;
		int      fd    = c->fd;
		uint32_t cap   = c->tx_cap;
		uint8_t *data  = c->tx_native + NET_RING_HDR_SIZE;

		if (c->tx_sent == 0) {
			uint32_t wi   = ring_ld32(c->tx_native + RING_OFF_WRITE);
			uint32_t used = wi - c->tx_ri;

			if (used < 2 || used > cap) {
				/* empty, or a write_idx the ring cannot hold */
				k_mutex_unlock(&g_mutex);
				return false;
			}

			uint16_t plen = (uint16_t)data[ring_off(c->tx_ri, 0, cap)] |
					((uint16_t)data[ring_off(c->tx_ri, 1, cap)] << 8);

			if (plen == 0) {
				tx_consume_locked(c, 2); /* zero-length sentinel */
				k_mutex_unlock(&g_mutex);
				continue;
			}
			if (2U + plen > used) {
				k_mutex_unlock(&g_mutex); /* incomplete — wait for more */
				return false;
			}
			if (plen > CONFIG_AKIRA_NET_MAX_MSG_SIZE) {
				NET_WRN("stream %d: TX message %u > %d bytes, dropped",
					h, plen, CONFIG_AKIRA_NET_MAX_MSG_SIZE);
				tx_consume_locked(c, 2U + plen);
				k_mutex_unlock(&g_mutex);
				continue;
			}
			c->tx_cur_len = plen;
		}

		uint32_t len = (uint32_t)c->tx_cur_len - c->tx_sent;

		ring_copy_out(g_tx_stage, data, cap,
			      ring_off(c->tx_ri, 2U + c->tx_sent, cap), len);
		k_mutex_unlock(&g_mutex);

		ssize_t ret = zsock_send(fd, g_tx_stage, len, ZSOCK_MSG_DONTWAIT);
		int err = (ret < 0) ? errno : 0;

		k_mutex_lock(&g_mutex, K_FOREVER);
		if (!ctx_live(c, gen)) {
			k_mutex_unlock(&g_mutex);
			return false;
		}
		if (c->tx_epoch != epoch || !c->tx_native) {
			/* Ring detached or re-bound while we were sending: what we
			 * sent belonged to the old ring, leave the new one alone. */
			k_mutex_unlock(&g_mutex);
			return false;
		}
		if (ret < 0) {
			if (err == EAGAIN || err == EWOULDBLOCK) {
				c->tx_wait_out = true;
			} else {
				NET_WRN("stream %d: send failed: %d", h, err);
				c->state = NET_STATE_ERROR;
				evt_post_locked(c, NET_EVT_ERROR, (uint16_t)err);
			}
			k_mutex_unlock(&g_mutex);
			return false;
		}

		c->tx_sent += (uint32_t)ret;
		if (c->tx_sent < c->tx_cur_len) {
			c->tx_wait_out = true; /* partial: resume when writable */
			k_mutex_unlock(&g_mutex);
			return false;
		}
		c->tx_sent = 0;
		tx_consume_locked(c, 2U + c->tx_cur_len);
		k_mutex_unlock(&g_mutex);
	}
	return true;
}

/* Peer closed or the socket failed: report once, stop polling. g_mutex held. */
static void mark_disconnected_locked(struct net_stream_ctx *c)
{
	c->state       = NET_STATE_CLOSED;
	c->tx_wait_out = false;
	evt_post_locked(c, NET_EVT_DISCONNECTED, 0);
}

static void rx_service(int h, int fd, uint32_t gen, short revents)
{
	struct net_stream_ctx *c = &g_streams[h];
	bool hup = (revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) != 0;
	size_t want = sizeof(g_rx_buf);

	k_mutex_lock(&g_mutex, K_FOREVER);
	if (!ctx_live(c, gen)) {
		k_mutex_unlock(&g_mutex);
		return;
	}

	uint8_t type = c->type;

	if (c->rx_native && type != NET_TYPE_UDP) {
		uint32_t room = rx_room_locked(c);

		if (room < 3) {
			/* No room: leave the bytes in the socket (backpressure).
			 * A hang-up we cannot drain ends the stream now. */
			if (hup) {
				mark_disconnected_locked(c);
			}
			k_mutex_unlock(&g_mutex);
			return;
		}
		want = MIN(want, room - 2);
	}
	k_mutex_unlock(&g_mutex);

	ssize_t n = zsock_recv(fd, g_rx_buf, want, ZSOCK_MSG_DONTWAIT);
	int err = (n < 0) ? errno : 0;

	if (n < 0 && (err == EAGAIN || err == EWOULDBLOCK) && !hup) {
		return; /* spurious wake-up */
	}

	k_mutex_lock(&g_mutex, K_FOREVER);
	if (!ctx_live(c, gen)) {
		k_mutex_unlock(&g_mutex);
		return;
	}
	if (n > 0) {
		if (!c->rx_native) {
			LOG_DBG("stream %d: no RX ring bound, %d bytes dropped", h, (int)n);
		} else if (rx_push_locked(c, g_rx_buf, (uint16_t)n) < 0) {
			/* UDP datagram larger than the free space (TCP reads are
			 * sized to fit, so only a concurrent re-bind gets here). */
			NET_WRN("stream %d: RX ring full, %d bytes dropped", h, (int)n);
		} else {
			evt_post_locked(c, NET_EVT_DATA_READY, 0);
		}
	} else if (n == 0 && type == NET_TYPE_UDP && !hup) {
		/* empty datagram */
	} else {
		mark_disconnected_locked(c);
	}
	k_mutex_unlock(&g_mutex);
}

static void accept_service(int h, int fd, uint32_t gen)
{
	int newfd = zsock_accept(fd, NULL, NULL); /* listening fd is non-blocking */

	if (newfd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			NET_WRN("stream %d: accept failed: %d", h, errno);
		}
		return;
	}

	int new_h = -ENOENT;

	k_mutex_lock(&g_mutex, K_FOREVER);
	struct net_stream_ctx *c = &g_streams[h];

	if (ctx_live(c, gen) && c->state == NET_STATE_LISTENING) {
		new_h = slot_alloc_locked(c->owner, NET_TYPE_TCP, newfd);
		if (new_h >= 0) {
			g_streams[new_h].state = NET_STATE_CONNECTED;
			evt_post_locked(c, NET_EVT_ACCEPT, (uint16_t)new_h);
		}
	}
	k_mutex_unlock(&g_mutex);

	if (new_h < 0) {
		/* Never in the table, so closing it here is safe. */
		NET_WRN("stream %d: cannot take connection (%d), closing it", h, new_h);
		zsock_close(newfd);
	} else {
		LOG_DBG("stream %d: accepted new conn -> handle %d", h, new_h);
	}
}

/* Close the sockets of streams marked closing (unless the connect worker
 * owns them) and free their slots; close the socket of a failed connect but
 * keep its slot for the owner to close. Only the poll thread closes stream
 * sockets, so a socket is never closed while this thread is inside a call on
 * it, and an fd number is never reused under its snapshot. */
static void reap_closing(void)
{
	int  fds[NET_MAX_STREAMS];
	int  hs[NET_MAX_STREAMS];
	bool free_slot[NET_MAX_STREAMS];
	int  n = 0;

	k_mutex_lock(&g_mutex, K_FOREVER);
	for (int i = 0; i < NET_MAX_STREAMS; i++) {
		struct net_stream_ctx *c = &g_streams[i];

		if (!c->used || c->conn_active || !(c->closing || c->drop_fd)) {
			continue;
		}
		if (c->closing) {
			c->conn_queued = false;
			if (c->fd < 0) {
				slot_free_locked(c);
				continue;
			}
			c->state = NET_STATE_CLOSED;
		} else if (c->fd < 0) {
			c->drop_fd = false;
			continue;
		}
		fds[n]       = c->fd;
		hs[n]        = i;
		free_slot[n] = c->closing;
		n++;
		c->fd      = -1;
		c->drop_fd = false;
	}
	k_mutex_unlock(&g_mutex);

	if (n == 0) {
		return;
	}
	for (int k = 0; k < n; k++) {
		zsock_close(fds[k]);
		LOG_DBG("stream %d: socket closed", hs[k]);
	}

	k_mutex_lock(&g_mutex, K_FOREVER);
	for (int k = 0; k < n; k++) {
		/* Still ours: closing slots are only ever freed here. */
		if (free_slot[k]) {
			slot_free_locked(&g_streams[hs[k]]);
		}
	}
	k_mutex_unlock(&g_mutex);
}

/* Wait events zsock_poll() needs for @p p. */
static int poll_events_of(const struct zsock_pollfd *p)
{
	return ((p->events & ZSOCK_POLLIN) ? 1 : 0) + ((p->events & ZSOCK_POLLOUT) ? 1 : 0);
}

/* zsock_poll() over @p nfds entries in batches that fit NET_POLL_EVENTS_MAX.
 * Only the last batch waits (and only if nothing was ready before it).
 * @return number of entries with events, or -1 if the first batch failed. */
static int poll_batched(struct zsock_pollfd *pfds, int nfds, int timeout_ms)
{
	int total = 0;
	int i = 0;

	while (i < nfds) {
		int cnt = 0;
		int ev  = 0;

		while (i + cnt < nfds) {
			int need = poll_events_of(&pfds[i + cnt]);

			if (cnt > 0 && ev + need > NET_POLL_EVENTS_MAX) {
				break;
			}
			ev += need;
			cnt++;
		}

		bool last = (i + cnt) >= nfds;
		int n = zsock_poll(&pfds[i], cnt, (last && total == 0) ? timeout_ms : 0);

		if (n < 0) {
			return (total > 0) ? total : -1;
		}
		total += n;
		i += cnt;
	}
	return total;
}

static void net_poll_thread_fn(void *a, void *b, void *c_)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c_);

	struct zsock_pollfd pfds[NET_MAX_STREAMS];
	int                 handles[NET_MAX_STREAMS];
	uint32_t            gens[NET_MAX_STREAMS];

	LOG_INF("Net poll thread started");

	while (1) {
		/* ---- 1. Close what was asked to close ---- */
		reap_closing();

		/* ---- 2. Drain TX rings (non-blocking) ---- */
		bool tx_more = false;

		for (int h = 0; h < NET_MAX_STREAMS; h++) {
			tx_more |= tx_drain(h);
		}

		/* ---- 3. Snapshot what to poll ---- */
		int  nfds = 0;
		bool any_live = false;

		k_mutex_lock(&g_mutex, K_FOREVER);
		for (int i = 0; i < NET_MAX_STREAMS; i++) {
			struct net_stream_ctx *c = &g_streams[i];
			short events = 0;

			if (!c->used || c->closing || c->fd < 0) {
				continue;
			}

			/* Only connected streams, listeners and UDP sockets are
			 * polled. An unconnected TCP/TLS socket has nothing to
			 * read, and one the connect worker owns is not ours to
			 * touch (polling one it is handshaking on would block this
			 * thread on its socket lock for the whole handshake). */
			bool data_stream = c->state == NET_STATE_CONNECTED ||
					   (c->state == NET_STATE_OPEN &&
					    c->type == NET_TYPE_UDP);

			if (c->state == NET_STATE_LISTENING) {
				events = ZSOCK_POLLIN;
			} else if (data_stream) {
				/* Parked streams (RX ring full, TX waiting for
				 * the app) need a re-check every tick. */
				any_live = true;
				if (!c->rx_native || rx_room_locked(c) >= 3) {
					events |= ZSOCK_POLLIN;
				}
				if (c->state == NET_STATE_CONNECTED && c->tx_wait_out) {
					events |= ZSOCK_POLLOUT;
				}
			}
			if (events == 0) {
				continue;
			}
			pfds[nfds].fd      = c->fd;
			pfds[nfds].events  = events;
			pfds[nfds].revents = 0;
			handles[nfds]      = i;
			gens[nfds]         = c->gen;
			nfds++;
		}
		k_mutex_unlock(&g_mutex);

		if (nfds == 0) {
			/* Nothing to poll. Streams that exist but are parked (RX ring
			 * full, TX waiting for the app) are re-checked every tick;
			 * with no stream at all, sleep until one opens. */
			k_sem_take(&g_poll_wake,
				   (any_live || tx_more) ? K_MSEC(NET_POLL_TICK_MS) : K_FOREVER);
			continue;
		}

		/* ---- 4. Poll ---- */
		int n = poll_batched(pfds, nfds, tx_more ? 0 : NET_POLL_TICK_MS);

		if (n < 0) {
			LOG_DBG("zsock_poll failed: %d", errno);
			k_sleep(K_MSEC(NET_POLL_TICK_MS)); /* never spin on a poll error */
			continue;
		}
		if (n == 0) {
			continue;
		}

		/* ---- 5. Process events ---- */
		for (int i = 0; i < nfds; i++) {
			short rev = pfds[i].revents;

			if (rev == 0) {
				continue;
			}

			int h = handles[i];
			struct net_stream_ctx *c = &g_streams[h];

			k_mutex_lock(&g_mutex, K_FOREVER);
			bool live = ctx_live(c, gens[i]);
			net_state_t state = c->state;

			if (live && (rev & ZSOCK_POLLOUT)) {
				c->tx_wait_out = false; /* writable: drain next pass */
			}
			k_mutex_unlock(&g_mutex);

			if (!live) {
				continue;
			}
			if (state == NET_STATE_LISTENING) {
				if (rev & ZSOCK_POLLIN) {
					accept_service(h, pfds[i].fd, gens[i]);
				}
				continue;
			}
			if (rev & (ZSOCK_POLLIN | ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
				rx_service(h, pfds[i].fd, gens[i], rev);
			}
		}
	}
}

/* =========================================================================
 * Connect worker (system workqueue, one connect at a time)
 * ======================================================================= */

/* Take the next queued connect: the system owner's first, then request
 * order. g_mutex held. */
static bool job_pick_locked(void)
{
	struct net_stream_ctx *best = NULL;

	for (int i = 0; i < NET_MAX_STREAMS; i++) {
		struct net_stream_ctx *c = &g_streams[i];

		if (!c->used || c->closing || !c->conn_queued) {
			continue;
		}
		bool c_sys = (c->owner == NET_OWNER_SYSTEM);

		if (!best ||
		    (c_sys && best->owner != NET_OWNER_SYSTEM) ||
		    (c_sys == (best->owner == NET_OWNER_SYSTEM) &&
		     (int32_t)(c->conn_seq - best->conn_seq) < 0)) {
			best = c;
		}
	}
	if (best) {
		best->conn_queued = false;
		best->conn_active = true;

		g_job.active = true;
		g_job.phase  = CP_START;
		g_job.type   = best->type;
		g_job.h      = ctx_handle(best);
		g_job.gen    = best->gen;
		g_job.fd     = best->fd;
		g_job.probe_fd = -1;
		g_job.result = 0;
		g_job.port   = best->port;
		memcpy(g_job.host, best->host, sizeof(g_job.host));
		g_job.host[sizeof(g_job.host) - 1] = '\0';
	}
	return best != NULL;
}

/* DNS (bounded by CONFIG_NET_SOCKETS_DNS_TIMEOUT); IPv4 literals skip it. */
static int job_resolve(void)
{
	memset(&g_job.addr, 0, sizeof(g_job.addr));
	g_job.addr.sin_family = AF_INET;
	g_job.addr.sin_port   = htons(g_job.port);

	if (zsock_inet_pton(AF_INET, g_job.host, &g_job.addr.sin_addr) == 1) {
		return 0;
	}

	struct zsock_addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = (g_job.type == NET_TYPE_UDP) ? SOCK_DGRAM : SOCK_STREAM,
	};
	struct zsock_addrinfo *res = NULL;
	int ret = zsock_getaddrinfo(g_job.host, NULL, &hints, &res);

	if (ret != 0 || !res || res->ai_family != AF_INET || !res->ai_addr) {
		NET_WRN("stream %d: DNS failed for \"%s\": %d", g_job.h, g_job.host, ret);
		if (res) {
			zsock_freeaddrinfo(res);
		}
		return -EHOSTUNREACH;
	}
	g_job.addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	zsock_freeaddrinfo(res);
	return 0;
}

/* Close the TLS reachability probe, if one is open. Worker-only. */
static void job_drop_probe(void)
{
	if (g_job.probe_fd >= 0) {
		zsock_close(g_job.probe_fd); /* never in the table: ours alone */
		g_job.probe_fd = -1;
	}
}

#ifdef CONFIG_AKIRA_WASM_NET_TLS
/* The stream being connected was closed/released (or its slot reused).
 * Never waits long for the table: if it is busy, assume still live — the
 * next run sees a cancellation either way. */
static bool job_cancelled(void)
{
	if (k_mutex_lock(&g_mutex, K_MSEC(NET_WORKER_LOCK_MS)) != 0) {
		return false;
	}
	bool cancelled = !ctx_live(&g_streams[g_job.h], g_job.gen);

	k_mutex_unlock(&g_mutex);
	return cancelled;
}

/* Reachability is known (the probe connected): set up TLS and connect for
 * real. Blocks for the TCP connect (about one round trip now) plus the
 * handshake, which Zephyr bounds by CONFIG_NET_SOCKETS_CONNECT_TIMEOUT. */
static int job_tls_connect(void)
{
	job_drop_probe();
	if (job_cancelled()) {
		return -ECANCELED;
	}

	sec_tag_t sec_tag_list[] = { NET_TLS_CA_SEC_TAG_GTS_R4, NET_TLS_CA_SEC_TAG_ISRG_X2,
				     NET_TLS_CA_SEC_TAG_GTS_R1 };
	int peer_verify = TLS_PEER_VERIFY_REQUIRED;

	if (zsock_setsockopt(g_job.fd, SOL_TLS, TLS_SEC_TAG_LIST,
			     sec_tag_list, sizeof(sec_tag_list)) < 0 ||
	    zsock_setsockopt(g_job.fd, SOL_TLS, TLS_HOSTNAME,
			     g_job.host, strlen(g_job.host)) < 0 ||
	    zsock_setsockopt(g_job.fd, SOL_TLS, TLS_PEER_VERIFY,
			     &peer_verify, sizeof(peer_verify)) < 0) {
		int err = errno;

		NET_WRN("stream %d: TLS setsockopt failed: %d", g_job.h, err);
		return -err;
	}

#ifdef CONFIG_AKIRA_WDT
	/* The workqueue is about to block in a bounded Zephyr call; start the
	 * watchdog window from here rather than from the last periodic feed. */
	akira_wdt_feed();
#endif
	if (zsock_connect(g_job.fd, (struct sockaddr *)&g_job.addr, sizeof(g_job.addr)) < 0) {
		return -errno;
	}
	return 0;
}
#endif /* CONFIG_AKIRA_WASM_NET_TLS */

/* A non-blocking TCP connect finished (@p phase says which one). */
static int job_tcp_done(uint8_t phase)
{
#ifdef CONFIG_AKIRA_WASM_NET_TLS
	if (phase == CP_PROBE_WAIT) {
		return job_tls_connect();
	}
#else
	ARG_UNUSED(phase);
#endif
	return 0;
}

/* Start a non-blocking TCP connect on @p fd. @return 1 = in progress. */
static int job_nb_connect(int fd, uint32_t timeout_ms, uint8_t phase)
{
	int ret = set_nonblock(fd);

	if (ret < 0) {
		return ret;
	}
	if (zsock_connect(fd, (struct sockaddr *)&g_job.addr, sizeof(g_job.addr)) == 0) {
		return job_tcp_done(phase);
	}
	if (errno != EINPROGRESS) {
		return -errno;
	}
	g_job.phase    = phase;
	g_job.deadline = k_uptime_get() + timeout_ms;
	return 1;
}

/* Check a non-blocking connect without waiting. @return 1 = still going. */
static int job_nb_check(int fd, uint8_t phase)
{
	struct zsock_pollfd p = { .fd = fd, .events = ZSOCK_POLLOUT };
	int n = zsock_poll(&p, 1, 0);

	if (n < 0) {
		return -errno;
	}
	if (n == 0 || p.revents == 0) {
		return (k_uptime_get() >= g_job.deadline) ? -ETIMEDOUT : 1;
	}

	int soerr = 0;
	socklen_t len = sizeof(soerr);

	if (zsock_getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr != 0) {
		return -soerr;
	}
	if (p.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
		return -ECONNREFUSED;
	}
	if (!(p.revents & ZSOCK_POLLOUT)) {
		return (k_uptime_get() >= g_job.deadline) ? -ETIMEDOUT : 1;
	}
	return job_tcp_done(phase);
}

/* Run the current phase. @return 1 = re-check later, 0 = connected, <0 = -errno. */
static int job_step(void)
{
	switch (g_job.phase) {
	case CP_START: {
		int ret = job_resolve();

		if (ret < 0) {
			return ret;
		}
		if (g_job.type == NET_TYPE_UDP) {
			/* Just sets the default peer. */
			if (zsock_connect(g_job.fd, (struct sockaddr *)&g_job.addr,
					  sizeof(g_job.addr)) < 0) {
				return -errno;
			}
			return 0;
		}
#ifdef CONFIG_AKIRA_WASM_NET_TLS
		if (g_job.type == NET_TYPE_TLS) {
			/* Zephyr's TLS connect is a blocking TCP connect plus a
			 * blocking handshake, each up to the connect timeout. Probe
			 * reachability with a non-blocking plain TCP connect first,
			 * so an unreachable host fails here, workqueue free, instead
			 * of holding the workqueue through the TCP timeout. */
			g_job.probe_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (g_job.probe_fd < 0) {
				return -errno;
			}
			return job_nb_connect(g_job.probe_fd, NET_TLS_PROBE_MS, CP_PROBE_WAIT);
		}
#endif
		/* If the stream was closed during DNS, the next run sees it and
		 * gives the socket back; connecting it meanwhile is harmless. */
		return job_nb_connect(g_job.fd, NET_TCP_CONNECT_MS, CP_TCP_WAIT);
	}
	case CP_TCP_WAIT:
		return job_nb_check(g_job.fd, CP_TCP_WAIT);
	case CP_PROBE_WAIT:
		return job_nb_check(g_job.probe_fd, CP_PROBE_WAIT);
	default:
		return -EINVAL;
	}
}

/* Record the outcome of the current job. g_mutex held. */
static void job_finish_locked(int err)
{
	struct net_stream_ctx *c = &g_streams[g_job.h];

	/* conn_active kept the slot from being freed: it is still ours. */
	if (c->used && c->gen == g_job.gen) {
		c->conn_active = false;
		if (c->closing) {
			/* Closed while connecting: the reaper closes the socket. */
		} else if (err == 0) {
			c->state = NET_STATE_CONNECTED;
			evt_post_locked(c, NET_EVT_CONNECTED, 0);
			LOG_DBG("stream %d: connected to \"%s\":%u",
				g_job.h, g_job.host, g_job.port);
		} else {
			/* The poll thread closes the socket (and frees its TLS
			 * context) right away; the slot stays until the owner
			 * closes the handle. */
			c->state   = NET_STATE_ERROR;
			c->drop_fd = true;
			evt_post_locked(c, NET_EVT_ERROR, (uint16_t)(-err));
			NET_WRN("stream %d: connect to \"%s\":%u failed: %d",
				g_job.h, g_job.host, g_job.port, -err);
		}
	}
	g_job.active = false;
}

static void connect_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Never block the system workqueue on the stream table: retry later. */
	if (k_mutex_lock(&g_mutex, K_MSEC(NET_WORKER_LOCK_MS)) != 0) {
		k_work_schedule(&g_conn_work, K_MSEC(NET_CONNECT_POLL_MS));
		return;
	}
	if (!g_job.active && !job_pick_locked()) {
		k_mutex_unlock(&g_mutex);
		return; /* nothing queued */
	}

	bool cancelled = !ctx_live(&g_streams[g_job.h], g_job.gen);

	k_mutex_unlock(&g_mutex);

	int ret;

	if (g_job.phase == CP_FINISH) {
		ret = g_job.result;
	} else if (cancelled) {
		ret = -ECANCELED;
	} else {
		ret = job_step();
	}

	if (ret > 0) {
		k_work_schedule(&g_conn_work, K_MSEC(NET_CONNECT_POLL_MS));
		return;
	}

	job_drop_probe(); /* done with it whatever the outcome */

	if (k_mutex_lock(&g_mutex, K_MSEC(NET_WORKER_LOCK_MS)) != 0) {
		g_job.phase  = CP_FINISH;
		g_job.result = ret;
		k_work_schedule(&g_conn_work, K_MSEC(NET_CONNECT_POLL_MS));
		return;
	}
	job_finish_locked(ret);
	k_mutex_unlock(&g_mutex);

	k_sem_give(&g_poll_wake); /* poll it, or reap it */

	/* Next queued connect (if any) in a fresh run, so other workqueue items
	 * get to run in between. */
	k_work_schedule(&g_conn_work, K_NO_WAIT);
}

/* =========================================================================
 * Public API
 * ======================================================================= */

int net_stream_init(void)
{
	if (k_mutex_lock(&g_init_lock, K_MSEC(NET_CALLER_LOCK_MS)) != 0) {
		return -EBUSY;
	}
	if (g_initialized) {
		k_mutex_unlock(&g_init_lock);
		return 0;
	}

#ifdef CONFIG_AKIRA_WASM_NET_TLS
	static const struct {
		sec_tag_t      tag;
		const uint8_t *cert;
		size_t         len;
		const char    *name;
	} cas[] = {
		{ NET_TLS_CA_SEC_TAG_GTS_R4, net_tls_ca_cert_gts_root_r4,
		  sizeof(net_tls_ca_cert_gts_root_r4), "GTS R4" },
		{ NET_TLS_CA_SEC_TAG_ISRG_X2, net_tls_ca_cert_isrg_root_x2,
		  sizeof(net_tls_ca_cert_isrg_root_x2), "ISRG X2" },
		{ NET_TLS_CA_SEC_TAG_GTS_R1, net_tls_ca_cert_gts_root_r1,
		  sizeof(net_tls_ca_cert_gts_root_r1), "GTS R1" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cas); i++) {
		int cret = tls_credential_add(cas[i].tag, TLS_CREDENTIAL_CA_CERTIFICATE,
					      cas[i].cert, cas[i].len);

		if (cret < 0 && cret != -EEXIST) {
			LOG_ERR("Failed to register TLS CA credential (%s): %d",
				cas[i].name, cret);
			k_mutex_unlock(&g_init_lock);
			return cret;
		}
	}
#endif

	memset(g_streams, 0, sizeof(g_streams));
	for (int i = 0; i < NET_MAX_STREAMS; i++) {
		g_streams[i].fd = -1;
	}
	memset(g_evt, 0, sizeof(g_evt));
	memset(&g_job, 0, sizeof(g_job));
	g_job.fd       = -1;
	g_job.probe_fd = -1;

	/* Created suspended so it can be pinned: a ready thread can't be. */
	k_thread_create(&g_poll_thread, g_poll_stack,
			K_THREAD_STACK_SIZEOF(g_poll_stack),
			net_poll_thread_fn, NULL, NULL, NULL,
			CONFIG_AKIRA_WASM_APP_PRIORITY, 0, K_FOREVER);
	k_thread_name_set(&g_poll_thread, "akira_net_poll");

	/* Under SMP, this thread's zsock_recv()/zsock_poll() calls (and the
	 * connect worker, on k_sys_work_q) drop straight into the WiFi blob's
	 * send/recv path via net_if_try_queue_tx() — CONFIG_NET_TC_TX_COUNT=0
	 * means there's no TX thread for net_tc.c's own core0 pin to apply to,
	 * so without this, TLS traffic runs on whichever core the scheduler
	 * picks. The WiFi blob is closed-source and, per its own Kconfig help
	 * text, expects every WiFi-facing thread pinned to core0 like the RX
	 * thread already is (net_tc.c). Confirmed root cause of SMP-only TLS
	 * fetch failures. */
#if defined(CONFIG_SCHED_CPU_MASK) && defined(CONFIG_SMP)
	k_thread_cpu_pin(&g_poll_thread, 0);
	k_thread_cpu_pin(&k_sys_work_q.thread, 0);
#endif
	k_thread_start(&g_poll_thread);

	g_initialized = true;
	k_mutex_unlock(&g_init_lock);

	LOG_INF("Net stream subsystem initialised (max_streams=%d, per app %d)",
		NET_MAX_STREAMS, NET_STREAMS_PER_APP);
	return 0;
}

int net_stream_open(int owner, int type)
{
#ifdef CONFIG_AKIRA_WASM_NET_TLS
	if (type != NET_TYPE_TCP && type != NET_TYPE_UDP && type != NET_TYPE_TLS) {
		return -EINVAL;
	}
#else
	if (type != NET_TYPE_TCP && type != NET_TYPE_UDP) {
		return -EINVAL;
	}
#endif
	if (!owner_valid(owner)) {
		return -EINVAL;
	}

	int ret = net_stream_init();

	if (ret < 0) {
		return ret;
	}

	/* Refuse before creating a socket an app at its cap could churn. */
	if (caller_lock() != 0) {
		return -EBUSY;
	}
	bool full = owner_at_cap_locked(owner);

	k_mutex_unlock(&g_mutex);
	if (full) {
		return -ENOSPC;
	}

	int sock_type = (type == NET_TYPE_UDP) ? SOCK_DGRAM : SOCK_STREAM;
	int proto     = (type == NET_TYPE_UDP) ? IPPROTO_UDP : IPPROTO_TCP;
	const char *type_name = (type == NET_TYPE_TCP) ? "TCP" :
				(type == NET_TYPE_UDP) ? "UDP" : "TLS";

#ifdef CONFIG_AKIRA_WASM_NET_TLS
	if (type == NET_TYPE_TLS) {
		proto = IPPROTO_TLS_1_2;
	}
#endif

	int fd = zsock_socket(AF_INET, sock_type, proto);

	if (fd < 0) {
		int err = errno;

		NET_WRN("zsock_socket(%s) failed: %d", type_name, err);
		return -err;
	}

	int h;

	if (caller_lock() != 0) {
		h = -EBUSY;
	} else {
		h = slot_alloc_locked(owner, type, fd);
		k_mutex_unlock(&g_mutex);
	}

	if (h < 0) {
		zsock_close(fd); /* never in the table: nobody else knows it */
		NET_WRN("owner %d: no free stream slot (%d)", owner, h);
		return h;
	}

	k_sem_give(&g_poll_wake);
	LOG_DBG("stream %d opened (%s fd=%d owner=%d)", h, type_name, fd, owner);
	return h;
}

int net_stream_connect(int owner, int handle, const char *host, uint16_t port)
{
	if (!host || !port) {
		return -EINVAL;
	}

	size_t hlen = strnlen(host, NET_STREAM_HOST_MAX + 1);

	if (hlen == 0) {
		return -EINVAL;
	}
	if (hlen > NET_STREAM_HOST_MAX) {
		return -ENAMETOOLONG; /* never connect to a silently truncated name */
	}

	if (caller_lock() != 0) {
		return -EBUSY;
	}
	struct net_stream_ctx *c = owned_locked(owner, handle);

	if (!c) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}
	if (c->state == NET_STATE_CONNECTING || c->state == NET_STATE_CONNECTED) {
		k_mutex_unlock(&g_mutex);
		return -EALREADY;
	}
	if (c->state != NET_STATE_OPEN || c->fd < 0) {
		k_mutex_unlock(&g_mutex);
		return -EINVAL;
	}

	memcpy(c->host, host, hlen);
	c->host[hlen]  = '\0';
	c->port        = port;
	c->state       = NET_STATE_CONNECTING;
	c->conn_queued = true;
	c->conn_seq    = ++g_conn_seq;
	uint32_t gen   = c->gen;

	k_mutex_unlock(&g_mutex);

	int ret = k_work_schedule(&g_conn_work, K_NO_WAIT);

	if (ret < 0) {
		if (caller_lock() == 0) {
			if (ctx_live(c, gen) && c->conn_queued) {
				c->conn_queued = false;
				c->state = NET_STATE_OPEN;
			}
			k_mutex_unlock(&g_mutex);
		}
		NET_WRN("stream %d: cannot queue connect: %d", handle, ret);
		return -EAGAIN;
	}

	LOG_DBG("stream %d: connect to \"%s\":%u queued", handle, host, port);
	return 0;
}

int net_stream_bind(int owner, int handle, uint16_t port)
{
	if (caller_lock() != 0) {
		return -EBUSY;
	}
	struct net_stream_ctx *c = owned_locked(owner, handle);

	if (!c) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}
	if (c->state != NET_STATE_OPEN || c->fd < 0) {
		k_mutex_unlock(&g_mutex);
		return -EINVAL;
	}

	/* Held across the (non-blocking) bind so the socket cannot be closed
	 * and its fd number reused underneath us. */
	int reuse = 1;
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr   = { .s_addr = INADDR_ANY },
		.sin_port   = htons(port),
	};

	(void)zsock_setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	int ret = zsock_bind(c->fd, (struct sockaddr *)&addr, sizeof(addr));
	int err = (ret < 0) ? errno : 0;

	k_mutex_unlock(&g_mutex);

	if (ret < 0) {
		NET_WRN("stream %d: bind to port %u failed: %d", handle, port, err);
		return -err;
	}

	k_sem_give(&g_poll_wake); /* a bound UDP socket becomes pollable */
	LOG_DBG("stream %d: bound to port %u", handle, port);
	return 0;
}

int net_stream_listen(int owner, int handle, int backlog)
{
	if (caller_lock() != 0) {
		return -EBUSY;
	}
	struct net_stream_ctx *c = owned_locked(owner, handle);

	if (!c) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}
	if (c->type != NET_TYPE_TCP) {
		k_mutex_unlock(&g_mutex);
		return -EPROTOTYPE;
	}
	if (c->state != NET_STATE_OPEN || c->fd < 0) {
		k_mutex_unlock(&g_mutex);
		return -EINVAL;
	}

	/* Non-blocking so the poll thread's accept() can never wait. */
	int ret = set_nonblock(c->fd);

	if (ret == 0 && zsock_listen(c->fd, CLAMP(backlog, 1, NET_MAX_STREAMS)) < 0) {
		ret = -errno;
	}
	if (ret == 0) {
		c->state = NET_STATE_LISTENING;
	}
	k_mutex_unlock(&g_mutex);

	if (ret < 0) {
		NET_WRN("stream %d: listen failed: %d", handle, ret);
		return ret;
	}

	k_sem_give(&g_poll_wake); /* a listening socket is pollable for accepts */
	LOG_DBG("stream %d: listening (backlog=%d)", handle, backlog);
	return 0;
}

int net_stream_close(int owner, int handle)
{
	if (caller_lock() != 0) {
		return -EBUSY;
	}
	struct net_stream_ctx *c = owned_locked(owner, handle);

	if (!c) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}

	/* Detach the rings now: the host never touches them again. No event for
	 * this handle can be posted after this (closing streams post none). */
	c->tx_native   = NULL;
	c->rx_native   = NULL;
	c->conn_queued = false;
	evt_purge_handle(owner, handle);

	if (c->fd < 0 && !c->conn_active) {
		slot_free_locked(c); /* nothing left to close */
		k_mutex_unlock(&g_mutex);
		LOG_DBG("stream %d: closed", handle);
		return 0;
	}

	/* The poll thread (after the connect worker lets go, if it holds it)
	 * closes the socket and frees the slot. */
	c->closing = true;
	uint32_t gen = c->gen;
	bool wait = !c->conn_active;

	k_mutex_unlock(&g_mutex);
	k_sem_give(&g_poll_wake);

	if (wait) {
		/* Bounded and fixed: lets a following open reuse the socket. The
		 * gen read is unlocked; it only decides when to stop waiting. */
		for (int t = 0; t < NET_CLOSE_WAIT_MS; t += NET_POLL_TICK_MS) {
			k_sleep(K_MSEC(NET_POLL_TICK_MS));
			if (*(volatile uint32_t *)&c->gen != gen) {
				break;
			}
		}
	}
	return 0;
}

static int ring_bind(int owner, int handle, uint8_t *native_ptr, uint32_t total_size, bool tx)
{
	if (!native_ptr || total_size <= NET_RING_HDR_SIZE) {
		return -EINVAL;
	}

	uint32_t cap = total_size - NET_RING_HDR_SIZE;

	if (caller_lock() != 0) {
		return -EBUSY;
	}
	struct net_stream_ctx *c = owned_locked(owner, handle);

	if (!c) {
		k_mutex_unlock(&g_mutex);
		return -ENOENT;
	}

	/* Header written under g_mutex: the poll thread never sees a half-set-up
	 * ring, and the old ring (if any) is no longer used once we unlock. */
	ring_hdr_init(native_ptr, cap);
	if (tx) {
		c->tx_native   = native_ptr;
		c->tx_cap      = cap;
		c->tx_epoch++;
		c->tx_ri       = 0;
		c->tx_sent     = 0;
		c->tx_cur_len  = 0;
		c->tx_wait_out = false;
	} else {
		c->rx_native = native_ptr;
		c->rx_cap    = cap;
		c->rx_wi     = 0;
	}
	k_mutex_unlock(&g_mutex);

	k_sem_give(&g_poll_wake);
	LOG_DBG("stream %d: %s ring bound (%u bytes data area)", handle, tx ? "TX" : "RX", cap);
	return 0;
}

int net_stream_tx_bind(int owner, int handle, uint8_t *native_ptr, uint32_t total_size)
{
	return ring_bind(owner, handle, native_ptr, total_size, true);
}

int net_stream_rx_bind(int owner, int handle, uint8_t *native_ptr, uint32_t total_size)
{
	return ring_bind(owner, handle, native_ptr, total_size, false);
}

int net_stream_tx_flush(int owner, int handle)
{
	if (caller_lock() != 0) {
		return -EBUSY;
	}
	struct net_stream_ctx *c = owned_locked(owner, handle);
	int ret = 0;

	if (!c) {
		ret = -ENOENT;
	} else if (c->state != NET_STATE_CONNECTED) {
		ret = -ENOTCONN;
	}
	k_mutex_unlock(&g_mutex);

	if (ret == 0) {
		k_sem_give(&g_poll_wake);
	}
	return ret;
}

int net_stream_event_pop(int owner, struct net_event *evt)
{
	if (!evt || !owner_valid(owner)) {
		return -EINVAL;
	}
	if (!g_initialized) {
		return NET_EVT_NONE;
	}
	return evt_pop(owner, evt);
}

void net_stream_release_owner(int owner)
{
	/* App slots only: the system owner's streams are never auto-released. */
	if (owner < 0 || owner >= AKIRA_MAX_WASM_INSTANCES || !g_initialized) {
		return;
	}

	/* Bounded: this runs on the stop path or the system workqueue. Only a
	 * thread that died holding g_mutex can keep it this long, and then
	 * nothing can ever take it to touch the rings again either. */
	if (k_mutex_lock(&g_mutex, K_MSEC(NET_RELEASE_LOCK_MS)) != 0) {
		LOG_ERR("app slot %d: stream table locked, streams not released", owner);
		return;
	}

	int released = 0;

	for (int i = 0; i < NET_MAX_STREAMS; i++) {
		struct net_stream_ctx *c = &g_streams[i];

		if (!c->used || c->owner != owner || c->closing) {
			continue;
		}
		c->tx_native   = NULL;
		c->rx_native   = NULL;
		c->conn_queued = false;
		if (c->fd < 0 && !c->conn_active) {
			slot_free_locked(c);
		} else {
			c->closing = true;
		}
		released++;
	}
	/* Under g_mutex: every event post is too, so none can land after this. */
	evt_purge(owner);
	k_mutex_unlock(&g_mutex);

	if (released > 0) {
		k_sem_give(&g_poll_wake);
		LOG_INF("app slot %d: released %d stream(s)", owner, released);
	}
}

#else /* !CONFIG_AKIRA_WASM_NET */

#include <errno.h>
#include "net_stream.h"

/* Stubs — return -ENOTSUP when the module is disabled */
int net_stream_init(void)                                            { return -ENOTSUP; }
int net_stream_open(int o, int t)                                    { (void)o; (void)t; return -ENOTSUP; }
int net_stream_connect(int o, int h, const char *s, uint16_t p)      { (void)o; (void)h; (void)s; (void)p; return -ENOTSUP; }
int net_stream_bind(int o, int h, uint16_t p)                        { (void)o; (void)h; (void)p; return -ENOTSUP; }
int net_stream_listen(int o, int h, int b)                           { (void)o; (void)h; (void)b; return -ENOTSUP; }
int net_stream_close(int o, int h)                                   { (void)o; (void)h; return -ENOTSUP; }
int net_stream_tx_bind(int o, int h, uint8_t *p, uint32_t s)         { (void)o; (void)h; (void)p; (void)s; return -ENOTSUP; }
int net_stream_rx_bind(int o, int h, uint8_t *p, uint32_t s)         { (void)o; (void)h; (void)p; (void)s; return -ENOTSUP; }
int net_stream_tx_flush(int o, int h)                                { (void)o; (void)h; return -ENOTSUP; }
int net_stream_event_pop(int o, struct net_event *e)                 { (void)o; (void)e; return -ENOTSUP; }
void net_stream_release_owner(int o)                                 { (void)o; }

#endif /* CONFIG_AKIRA_WASM_NET */
