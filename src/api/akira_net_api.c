/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_net_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_net_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_net_api.c
 * @brief WASM native bridge for the TCP/UDP Network API.
 *
 * Every function:
 *   1. Guards with AKIRA_CHECK_CAP_OR_RETURN (requires "network" capability).
 *   2. Validates WASM pointer arguments via wasm_runtime_validate_app_addr().
 *   3. Converts WASM pointers to native C pointers.
 *   4. Delegates to the net_stream engine.
 */

#include "akira_net_api.h"
#include "akira_api.h"
#include "runtime/security.h"
#include "connectivity/net/net_stream.h"

#ifdef CONFIG_AKIRA_WASM_NET

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <wasm_export.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

/* =========================================================================
 * Internal helpers (mirrors wasm_ptr_to_native in akira_ble_api.c)
 * ======================================================================= */

/**
 * @brief Validate a WASM pointer and return the native C equivalent.
 * @return Native pointer on success; NULL if out-of-bounds.
 */
static void *wasm_ptr_to_native(wasm_exec_env_t env, uint32_t ptr, uint32_t len)
{
	wasm_module_inst_t mi = wasm_runtime_get_module_inst(env);

	if (!mi) {
		return NULL;
	}
	if (!wasm_runtime_validate_app_addr(mi, ptr, len)) {
		return NULL;
	}
	return wasm_runtime_addr_app_to_native(mi, ptr);
}

/* =========================================================================
 * WASM native functions
 * ======================================================================= */

int akira_native_net_open(wasm_exec_env_t exec_env, int32_t type)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EACCES);

	if (type != NET_TYPE_TCP && type != NET_TYPE_UDP) {
		LOG_ERR("net_open: invalid type %d", type);
		return -EINVAL;
	}

	/* Lazy-start the poll thread on first use */
	int ret = net_stream_init();

	if (ret < 0) {
		LOG_ERR("net_open: net_stream_init failed: %d", ret);
		return ret;
	}

	return net_stream_open((int)type);
}

int akira_native_net_connect(wasm_exec_env_t exec_env, int32_t handle,
			     const char *host, int32_t port)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EACCES);

	if (!host || port <= 0 || port > 65535) {
		LOG_ERR("net_connect: bad args (host=%p port=%d)", host, port);
		return -EINVAL;
	}

	return net_stream_connect((int)handle, host, (uint16_t)port);
}

int akira_native_net_bind(wasm_exec_env_t exec_env, int32_t handle,
			  int32_t port)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EACCES);

	if (port < 0 || port > 65535) {
		return -EINVAL;
	}

	return net_stream_bind((int)handle, (uint16_t)port);
}

int akira_native_net_listen(wasm_exec_env_t exec_env, int32_t handle,
			    int32_t backlog)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EACCES);

	if (backlog <= 0) {
		backlog = 1;
	}

	return net_stream_listen((int)handle, (int)backlog);
}

int akira_native_net_close(wasm_exec_env_t exec_env, int32_t handle)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EACCES);

	return net_stream_close((int)handle);
}

int akira_native_net_tx_bind(wasm_exec_env_t exec_env, int32_t handle,
			     int32_t wasm_ptr, int32_t total_size)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EACCES);

	if (total_size <= NET_RING_HDR_SIZE) {
		LOG_ERR("net_tx_bind: buffer too small (%d <= %d)",
			total_size, NET_RING_HDR_SIZE);
		return -EINVAL;
	}

	uint8_t *native = wasm_ptr_to_native(exec_env,
					     (uint32_t)wasm_ptr,
					     (uint32_t)total_size);
	if (!native) {
		LOG_ERR("net_tx_bind: invalid WASM ptr 0x%x size %d",
			wasm_ptr, total_size);
		return -EFAULT;
	}

	return net_stream_tx_bind((int)handle, native, (uint32_t)total_size);
}

int akira_native_net_rx_bind(wasm_exec_env_t exec_env, int32_t handle,
			     int32_t wasm_ptr, int32_t total_size)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EACCES);

	if (total_size <= NET_RING_HDR_SIZE) {
		LOG_ERR("net_rx_bind: buffer too small (%d <= %d)",
			total_size, NET_RING_HDR_SIZE);
		return -EINVAL;
	}

	uint8_t *native = wasm_ptr_to_native(exec_env,
					     (uint32_t)wasm_ptr,
					     (uint32_t)total_size);
	if (!native) {
		LOG_ERR("net_rx_bind: invalid WASM ptr 0x%x size %d",
			wasm_ptr, total_size);
		return -EFAULT;
	}

	return net_stream_rx_bind((int)handle, native, (uint32_t)total_size);
}

int akira_native_net_tx_flush(wasm_exec_env_t exec_env, int32_t handle)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EACCES);

	return net_stream_tx_flush((int)handle);
}

int akira_native_net_event_pop(wasm_exec_env_t exec_env,
			       uint32_t buf_ptr, uint32_t len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EACCES);

	/* Minimum: 1 (type) + 1 (handle) + 2 (extra) = 4 bytes */
	if (len < 4) {
		return -EINVAL;
	}

	uint8_t *buf = (uint8_t *)wasm_ptr_to_native(exec_env, buf_ptr, len);

	if (!buf) {
		return -EFAULT;
	}

	struct net_event evt;
	int type = net_stream_event_pop(&evt);

	if (type <= NET_EVT_NONE) {
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

int akira_native_net_get_ip(wasm_exec_env_t exec_env,
			    uint32_t buf_ptr, uint32_t len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NETWORK, -EACCES);

	if (len < 16) {
		return -EINVAL;
	}

	char *buf = (char *)wasm_ptr_to_native(exec_env, buf_ptr, len);

	if (!buf) {
		return -EFAULT;
	}

	struct net_if *iface = net_if_get_default();

	if (!iface) {
		buf[0] = '\0';
		return -ENODATA;
	}

	struct in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

	if (!addr) {
		buf[0] = '\0';
		return -ENODATA;
	}

	net_addr_ntop(AF_INET, addr, buf, (size_t)len);
	return 0;
}

#else /* !CONFIG_AKIRA_WASM_NET */

int akira_native_net_open(wasm_exec_env_t e, int32_t t)
{ (void)e; (void)t; return -ENOTSUP; }
int akira_native_net_connect(wasm_exec_env_t e, int32_t h, const char *host, int32_t p)
{ (void)e; (void)h; (void)host; (void)p; return -ENOTSUP; }
int akira_native_net_bind(wasm_exec_env_t e, int32_t h, int32_t p)
{ (void)e; (void)h; (void)p; return -ENOTSUP; }
int akira_native_net_listen(wasm_exec_env_t e, int32_t h, int32_t b)
{ (void)e; (void)h; (void)b; return -ENOTSUP; }
int akira_native_net_close(wasm_exec_env_t e, int32_t h)
{ (void)e; (void)h; return -ENOTSUP; }
int akira_native_net_tx_bind(wasm_exec_env_t e, int32_t h, int32_t p, int32_t s)
{ (void)e; (void)h; (void)p; (void)s; return -ENOTSUP; }
int akira_native_net_rx_bind(wasm_exec_env_t e, int32_t h, int32_t p, int32_t s)
{ (void)e; (void)h; (void)p; (void)s; return -ENOTSUP; }
int akira_native_net_tx_flush(wasm_exec_env_t e, int32_t h)
{ (void)e; (void)h; return -ENOTSUP; }
int akira_native_net_event_pop(wasm_exec_env_t e, uint32_t b, uint32_t l)
{ (void)e; (void)b; (void)l; return -ENOTSUP; }
int akira_native_net_get_ip(wasm_exec_env_t e, uint32_t b, uint32_t l)
{ (void)e; (void)b; (void)l; return -ENOTSUP; }

#endif /* CONFIG_AKIRA_WASM_NET */
