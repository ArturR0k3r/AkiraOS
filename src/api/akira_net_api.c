/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_net_api
#include <akira_net_policy.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_net_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_net_api.c
 * @brief WASM native bridge for the TCP/UDP Network API.
 *
 * Every function:
 *   1. Guards with AKIRA_CHECK_CAP_OR_RETURN (requires "network" capability).
 *   2. Validates WASM pointer arguments over their full length.
 *   3. Asks the egress policy providers (akira_net_policy.h) before connect,
 *      bind and listen.
 *   4. Delegates to the net_stream engine as the calling app's slot (owner),
 *      inside a native critical section (see NET_CRITICAL).
 *
 * Streams, rings and queued events belong to the app's slot and are released
 * by net_release() however the app ends — first of all release hooks, since
 * the poll thread writes into the app's RX rings.
 */

#include "akira_net_api.h"
#include "akira_api.h"
#include "akira_wasm_mem.h"
#include "runtime/security.h"
#include "connectivity/net/net_stream.h"

#ifdef CONFIG_AKIRA_WASM_NET

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <wasm_export.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <akira_app_containment.h>
#include "runtime/akira_runtime.h" /* AKIRA_MAX_WASM_INSTANCES */

/* WASM pointer validation lives in akira_wasm_mem.h (akira_wasm_ptr_to_native). */
#define wasm_ptr_to_native(env, ptr, len) akira_wasm_ptr_to_native((env), (ptr), (len))

/* =========================================================================
 * Internal helpers
 * ======================================================================= */

/* App-triggered warnings: at most one a second, so a looping app cannot
 * flood the log. */
static bool net_api_log_allowed(void)
{
	static uint32_t last;
	uint32_t now = k_uptime_get_32();

	if (last != 0U && (now - last) < 1000U)
	{
		return false;
	}
	last = now ? now : 1U;
	return true;
}

#define NET_API_WRN(...)                                                       \
	do                                                                     \
	{                                                                      \
		if (net_api_log_allowed())                                     \
		{                                                              \
			LOG_WRN(__VA_ARGS__);                                  \
		}                                                              \
	} while (0)

/*
 * Every net_stream call below runs inside a native critical section: they
 * take the stream-table mutex and make (non-blocking) socket calls that hold
 * network-stack locks, and an app thread aborted while holding either would
 * wedge every stream and the stack itself. The calls are all bounded — none
 * waits on the network, the table lock gives up after a second — so a stop
 * is delayed by milliseconds at most.
 */
#define NET_CRITICAL(env, expr)                                                \
	({                                                                     \
		akira_native_critical_enter(env);                              \
		int net_crit_ret = (expr);                                     \
		akira_native_critical_exit(env);                               \
		net_crit_ret;                                                  \
	})

/*
 * Owner of the streams the calling app opens: its runtime slot. Every stream
 * is tagged with it; net_stream refuses calls on handles another slot holds,
 * and the slot's streams are closed by net_release() when the app goes away.
 * Also refuses new work for an app that is being stopped.
 */
static int net_owner(wasm_exec_env_t exec_env, bool new_work)
{
	int owner = akira_runtime_slot_of_exec_env(exec_env);

	if (owner < 0)
	{
		return -EACCES;
	}
	if (new_work && akira_runtime_stop_requested(exec_env))
	{
		return -ECANCELED;
	}
	return owner;
}

/*
 * The policy layer logs and audits every refusal. An app that keeps retrying
 * a refused request would flood both, so after NET_DENY_BURST refusals within
 * a second the app's further requests are refused here, without asking the
 * providers again, until that second is over (fail closed). Each entry is
 * touched only by its slot's own thread.
 */
#define NET_DENY_BURST     4
#define NET_DENY_WINDOW_MS 1000U

static struct {
	uint32_t start;
	uint8_t  count;
} g_deny[AKIRA_MAX_WASM_INSTANCES];

/* Ask the egress policy providers (include/akira_net_policy.h). */
static int net_policy_gate(wasm_exec_env_t exec_env, int owner, enum akira_net_op op,
			   const char *host, uint16_t port)
{
	uint32_t now = k_uptime_get_32();

	if ((now - g_deny[owner].start) >= NET_DENY_WINDOW_MS) {
		g_deny[owner].start = now;
		g_deny[owner].count = 0;
	} else if (g_deny[owner].count >= NET_DENY_BURST) {
		return -EACCES;
	}

	char app_name[32] = "";
	wasm_module_inst_t mi = wasm_runtime_get_module_inst(exec_env);

	if (mi) {
		(void)akira_runtime_get_name_for_module_inst(mi, app_name,
							     sizeof(app_name));
	}

	struct akira_net_policy_ctx ctx = {
		.op = op,
		.app_name = app_name,
		.host = host,
		.port = port,
	};

	int ret = akira_net_policy_check(&ctx);

	if (ret != 0) {
		g_deny[owner].count++;
	}
	return ret;
}

/**
 * @brief True if the app's linear memory can never move.
 *
 * The poll thread keeps native pointers to ring buffers in linear memory and
 * uses them from its own thread. WAMR reallocates linear memory on
 * memory.grow (os_mremap / realloc on Zephyr), so a growable memory could
 * move underneath it — a use-after-free no lock can prevent, since
 * memory.grow runs inside the app's code without any hook. Once cur pages ==
 * max pages, memory.grow always fails and the base stays put until the
 * instance is freed, before which net_release() detaches the rings. The SDK
 * Makefiles link apps with --initial-memory == --max-memory, so this holds
 * for them.
 */
static bool linear_memory_is_fixed(wasm_exec_env_t exec_env)
{
	wasm_module_inst_t mi = wasm_runtime_get_module_inst(exec_env);
	wasm_memory_inst_t mem = mi ? wasm_runtime_get_default_memory(mi) : NULL;

	if (!mem)
	{
		return false;
	}

	uint64_t cur = wasm_memory_get_cur_page_count(mem);
	uint64_t max = wasm_memory_get_max_page_count(mem);

	if (cur < max)
	{
		NET_API_WRN("net ring bind refused: linear memory can grow (%u of %u pages); "
			    "link the app with --max-memory equal to --initial-memory",
			    (unsigned int)cur, (unsigned int)max);
		return false;
	}
	return true;
}

/* =========================================================================
 * WASM native functions
 * ======================================================================= */

int akira_native_net_open(wasm_exec_env_t exec_env, int32_t type)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EPERM);

	if (type != NET_TYPE_TCP && type != NET_TYPE_UDP)
	{
		NET_API_WRN("net_open: invalid type %d", type);
		return -EINVAL;
	}

	int owner = net_owner(exec_env, true);

	if (owner < 0)
	{
		return owner;
	}

	/* Lazy-starts the poll thread on first use. */
	return NET_CRITICAL(exec_env, net_stream_open(owner, (int)type));
}

int akira_native_net_connect(wasm_exec_env_t exec_env, int32_t handle,
			     const char *host, int32_t port)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EPERM);

	if (!host || port <= 0 || port > 65535)
	{
		return -EINVAL;
	}

	/*
	 * WAMR checked that "$" is NUL-terminated inside linear memory. Copy it
	 * once so the policy decides on exactly the name that is connected to:
	 * an over-long name is refused, never truncated after the check (a
	 * provider matching a suffix would otherwise approve a different host).
	 */
	char name[NET_STREAM_HOST_MAX + 1];
	size_t hlen = strnlen(host, sizeof(name));

	if (hlen == 0)
	{
		return -EINVAL;
	}
	if (hlen >= sizeof(name))
	{
		return -ENAMETOOLONG;
	}
	memcpy(name, host, hlen);
	name[hlen] = '\0';

	int owner = net_owner(exec_env, true);

	if (owner < 0)
	{
		return owner;
	}

	int policy_ret = net_policy_gate(exec_env, owner, AKIRA_NET_OP_CONNECT, name,
					 (uint16_t)port);
	if (policy_ret != 0)
	{
		return policy_ret;
	}

	return NET_CRITICAL(exec_env,
			    net_stream_connect(owner, (int)handle, name, (uint16_t)port));
}

int akira_native_net_bind(wasm_exec_env_t exec_env, int32_t handle,
			  int32_t port)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EPERM);

	if (port < 0 || port > 65535)
	{
		return -EINVAL;
	}

	int owner = net_owner(exec_env, true);

	if (owner < 0)
	{
		return owner;
	}

	int policy_ret = net_policy_gate(exec_env, owner, AKIRA_NET_OP_BIND, NULL,
					 (uint16_t)port);
	if (policy_ret != 0)
	{
		return policy_ret;
	}

	return NET_CRITICAL(exec_env, net_stream_bind(owner, (int)handle, (uint16_t)port));
}

int akira_native_net_listen(wasm_exec_env_t exec_env, int32_t handle,
			    int32_t backlog)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EPERM);

	if (backlog <= 0)
	{
		backlog = 1;
	}

	int owner = net_owner(exec_env, true);

	if (owner < 0)
	{
		return owner;
	}

	int policy_ret = net_policy_gate(exec_env, owner, AKIRA_NET_OP_LISTEN, NULL, 0);
	if (policy_ret != 0)
	{
		return policy_ret;
	}

	return NET_CRITICAL(exec_env, net_stream_listen(owner, (int)handle, (int)backlog));
}

int akira_native_net_close(wasm_exec_env_t exec_env, int32_t handle)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EPERM);

	int owner = net_owner(exec_env, false);

	if (owner < 0)
	{
		return owner;
	}

	/* Waits at most ~200 ms (fixed, not app-chosen) for the socket close. */
	return NET_CRITICAL(exec_env, net_stream_close(owner, (int)handle));
}

static int net_ring_bind(wasm_exec_env_t exec_env, int32_t handle,
			 int32_t wasm_ptr, int32_t total_size, bool tx)
{
	const char *what = tx ? "net_tx_bind" : "net_rx_bind";

	if (total_size <= NET_RING_HDR_SIZE)
	{
		NET_API_WRN("%s: buffer too small (%d <= %d)", what, total_size,
			    NET_RING_HDR_SIZE);
		return -EINVAL;
	}

	int owner = net_owner(exec_env, true);

	if (owner < 0)
	{
		return owner; /* -ECANCELED: the instance is about to be freed */
	}

	uint8_t *native = wasm_ptr_to_native(exec_env, (uint32_t)wasm_ptr, (uint32_t)total_size);

	if (!native)
	{
		NET_API_WRN("%s: invalid WASM ptr 0x%x size %d", what, wasm_ptr, total_size);
		return -EFAULT;
	}
	if (!linear_memory_is_fixed(exec_env))
	{
		return -ENOTSUP;
	}

	return NET_CRITICAL(exec_env,
			    tx ? net_stream_tx_bind(owner, (int)handle, native,
						    (uint32_t)total_size)
			       : net_stream_rx_bind(owner, (int)handle, native,
						    (uint32_t)total_size));
}

int akira_native_net_tx_bind(wasm_exec_env_t exec_env, int32_t handle,
			     int32_t wasm_ptr, int32_t total_size)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EPERM);

	return net_ring_bind(exec_env, handle, wasm_ptr, total_size, true);
}

int akira_native_net_rx_bind(wasm_exec_env_t exec_env, int32_t handle,
			     int32_t wasm_ptr, int32_t total_size)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EPERM);

	return net_ring_bind(exec_env, handle, wasm_ptr, total_size, false);
}

int akira_native_net_tx_flush(wasm_exec_env_t exec_env, int32_t handle)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EPERM);

	int owner = net_owner(exec_env, false);

	if (owner < 0)
	{
		return owner;
	}

	/*
	 * Only wakes the poll thread, which sends non-blocking: sending never
	 * happens on the app thread, so a stalled peer cannot block the app.
	 * Returns 0 rather than a byte count (the bytes leave asynchronously);
	 * send errors arrive as NET_EVT_ERROR.
	 */
	return NET_CRITICAL(exec_env, net_stream_tx_flush(owner, (int)handle));
}

int akira_native_net_event_pop(wasm_exec_env_t exec_env,
			       uint32_t buf_ptr, uint32_t len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EPERM);

	/* Minimum: 1 (type) + 1 (handle) + 2 (extra) = 4 bytes */
	if (len < 4)
	{
		return -EINVAL;
	}

	uint8_t *buf = (uint8_t *)wasm_ptr_to_native(exec_env, buf_ptr, len);

	if (!buf)
	{
		return -EFAULT;
	}

	int owner = net_owner(exec_env, false);

	if (owner < 0)
	{
		return owner;
	}

	/* Only this app's events; the pop takes a spinlock, nothing an abort
	 * could leave held, so no critical section is needed. */
	struct net_event evt;
	int type = net_stream_event_pop(owner, &evt);

	if (type <= NET_EVT_NONE)
	{
		return NET_EVT_NONE;
	}

	/*
	 * Serialise into WASM buffer (layout mirrors the BLE event format):
	 *   [0]   type   (uint8)
	 *   [1]   handle (uint8)
	 *   [2-3] extra  (uint16 LE)
	 */
	buf[0] = evt.type;
	buf[1] = evt.handle;
	buf[2] = (uint8_t)(evt.extra & 0xFF);
	buf[3] = (uint8_t)(evt.extra >> 8);

	return type;
}

/* Read the address under the interface lock, format it outside. */
static int net_get_ip_locked(struct in_addr *out)
{
	struct net_if *iface = net_if_get_wifi_sta();

	if (!iface)
	{
		return -ENODATA;
	}

	struct in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

	if (!addr)
	{
		return -ENODATA;
	}
	*out = *addr;
	return 0;
}

int akira_native_net_get_ip(wasm_exec_env_t exec_env,
			    uint32_t buf_ptr, uint32_t len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EPERM);

	if (len < 16)
	{
		return -EINVAL;
	}

	char *buf = (char *)wasm_ptr_to_native(exec_env, buf_ptr, len);

	if (!buf)
	{
		return -EFAULT;
	}

	/* The interface lookups take net_if locks: no abort inside. */
	struct in_addr addr;
	int ret = NET_CRITICAL(exec_env, net_get_ip_locked(&addr));

	if (ret < 0)
	{
		buf[0] = '\0';
		return ret;
	}

	/* "255.255.255.255" + NUL fits the 16 bytes checked above. */
	if (!net_addr_ntop(AF_INET, &addr, buf, (size_t)MIN(len, 16U)))
	{
		buf[0] = '\0';
		return -ENODATA;
	}
	return 0;
}

/* =========================================================================
 * App teardown
 * ======================================================================= */

/*
 * Order 10, first of all release hooks: the poll thread copies received data
 * into the app's RX rings, and nothing may write into the app's memory once
 * it can be freed. Detaches every ring and drops the slot's events under the
 * stream-table lock; the sockets are closed asynchronously by the poll
 * thread. Never blocks for long, so it is safe on the stop path and the
 * system workqueue.
 */
static void net_release(int slot, const char *app_name)
{
	ARG_UNUSED(app_name);
	net_stream_release_owner(slot);
}

AKIRA_APP_RESOURCE_DEFINE(net, 10, NULL, net_release);

#else /* !CONFIG_AKIRA_WASM_NET */

int akira_native_net_open(wasm_exec_env_t e, int32_t t)
{
	(void)e;
	(void)t;
	return -ENOTSUP;
}
int akira_native_net_connect(wasm_exec_env_t e, int32_t h, const char *host, int32_t p)
{
	(void)e;
	(void)h;
	(void)host;
	(void)p;
	return -ENOTSUP;
}
int akira_native_net_bind(wasm_exec_env_t e, int32_t h, int32_t p)
{
	(void)e;
	(void)h;
	(void)p;
	return -ENOTSUP;
}
int akira_native_net_listen(wasm_exec_env_t e, int32_t h, int32_t b)
{
	(void)e;
	(void)h;
	(void)b;
	return -ENOTSUP;
}
int akira_native_net_close(wasm_exec_env_t e, int32_t h)
{
	(void)e;
	(void)h;
	return -ENOTSUP;
}
int akira_native_net_tx_bind(wasm_exec_env_t e, int32_t h, int32_t p, int32_t s)
{
	(void)e;
	(void)h;
	(void)p;
	(void)s;
	return -ENOTSUP;
}
int akira_native_net_rx_bind(wasm_exec_env_t e, int32_t h, int32_t p, int32_t s)
{
	(void)e;
	(void)h;
	(void)p;
	(void)s;
	return -ENOTSUP;
}
int akira_native_net_tx_flush(wasm_exec_env_t e, int32_t h)
{
	(void)e;
	(void)h;
	return -ENOTSUP;
}
int akira_native_net_event_pop(wasm_exec_env_t e, uint32_t b, uint32_t l)
{
	(void)e;
	(void)b;
	(void)l;
	return -ENOTSUP;
}
int akira_native_net_get_ip(wasm_exec_env_t e, uint32_t b, uint32_t l)
{
	(void)e;
	(void)b;
	(void)l;
	return -ENOTSUP;
}

#endif /* CONFIG_AKIRA_WASM_NET */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_NET))
#include <akira_native_registry.h>

static const NativeSymbol akira_net_natives[] = {
    {"net_open", (void *)akira_native_net_open, "(i)i", NULL},
    {"net_connect", (void *)akira_native_net_connect, "(i$i)i", NULL},
    {"net_bind", (void *)akira_native_net_bind, "(ii)i", NULL},
    {"net_listen", (void *)akira_native_net_listen, "(ii)i", NULL},
    {"net_close", (void *)akira_native_net_close, "(i)i", NULL},
    {"net_tx_bind", (void *)akira_native_net_tx_bind, "(iii)i", NULL},
    {"net_rx_bind", (void *)akira_native_net_rx_bind, "(iii)i", NULL},
    {"net_tx_flush", (void *)akira_native_net_tx_flush, "(i)i", NULL},
    {"net_event_pop", (void *)akira_native_net_event_pop, "(ii)i", NULL},
    {"net_get_ip", (void *)akira_native_net_get_ip, "(ii)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_net_api, "env", akira_net_natives);
#endif
