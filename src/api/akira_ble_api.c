/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_ble_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ble_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_ble_api.c
 * @brief Arduino-style BLE WASM native API implementation.
 *
 * Bridges WASM BLE calls to the ble_app_service layer.
 * All functions are guarded by AKIRA_CAP_BLE.
 */

#include "akira_api.h"
#include "akira_ble_api.h"
#include "runtime/security.h"

#ifdef CONFIG_AKIRA_WASM_BLE

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include "connectivity/bluetooth/ble_app_service.h"
#include "connectivity/bluetooth/bt_manager.h"

#ifdef CONFIG_BT
#include <zephyr/bluetooth/bluetooth.h>
#endif

/*
 * Track which service UUID should be included in the advertisement.
 * Set by ble_set_advertised_service(); consumed by ble_advertise().
 */
static int g_adv_svc_handle = -1;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/** Resolve WASM pointer to native buffer with length check. */
static void *wasm_ptr_to_native(wasm_exec_env_t env,
				uint32_t ptr, uint32_t len)
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

/* ------------------------------------------------------------------ */
/* WASM-exported native functions                                      */
/* ------------------------------------------------------------------ */

int akira_native_ble_init(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	int ret = bt_manager_set_mode(BT_MODE_BLE_APP);

	if (ret < 0) {
		LOG_ERR("ble_init: set_mode failed: %d", ret);
		return ret;
	}

	ret = ble_app_svc_init();
	if (ret < 0) {
		LOG_ERR("ble_init: svc_init failed: %d", ret);
		return ret;
	}

	g_adv_svc_handle = -1;
	LOG_INF("BLE app mode initialised");
	return 0;
}

int akira_native_ble_deinit(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	bt_manager_stop_advertising();
	ble_app_svc_deinit();
	bt_manager_set_mode(BT_MODE_NONE);
	g_adv_svc_handle = -1;

	LOG_INF("BLE app mode released");
	return 0;
}

int akira_native_ble_set_local_name(wasm_exec_env_t exec_env,
				    const char *name)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	if (!name) {
		return -EINVAL;
	}

#ifdef CONFIG_BT
	int ret = bt_set_name(name);

	if (ret < 0) {
		LOG_ERR("bt_set_name failed: %d", ret);
		return ret;
	}
#endif

	LOG_INF("BLE local name set to \"%s\"", name);
	return 0;
}

int akira_native_ble_service_create(wasm_exec_env_t exec_env,
				    const char *uuid128_str)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	if (!uuid128_str) {
		return -EINVAL;
	}

	return ble_app_svc_alloc(uuid128_str);
}

int akira_native_ble_char_create(wasm_exec_env_t exec_env,
				 const char *uuid128_str,
				 int32_t props,
				 int32_t max_len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	if (!uuid128_str || max_len <= 0) {
		return -EINVAL;
	}

	return ble_app_char_alloc(uuid128_str,
				  (uint8_t)props,
				  (uint16_t)max_len);
}

int akira_native_ble_service_add_char(wasm_exec_env_t exec_env,
				      int32_t svc_h, int32_t char_h)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	return ble_app_svc_add_char(svc_h, char_h);
}

int akira_native_ble_add_service(wasm_exec_env_t exec_env, int32_t svc_h)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	return ble_app_svc_register((int)svc_h);
}

int akira_native_ble_set_advertised_service(wasm_exec_env_t exec_env,
					    int32_t svc_h)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	g_adv_svc_handle = (int)svc_h;
	LOG_DBG("Advertised service handle set to %d", g_adv_svc_handle);
	return 0;
}

int akira_native_ble_advertise(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	if (bt_manager_get_mode() != BT_MODE_BLE_APP) {
		LOG_ERR("ble_advertise: not in BLE_APP mode (call ble_init first)");
		return -EINVAL;
	}

	const uint8_t *uuid128 = NULL;

	if (g_adv_svc_handle >= 0) {
		uuid128 = ble_app_svc_get_uuid128(g_adv_svc_handle);
	}

	return bt_manager_start_advertising_custom(uuid128);
}

int akira_native_ble_stop_advertise(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	return bt_manager_stop_advertising();
}

int akira_native_ble_is_connected(wasm_exec_env_t exec_env)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	return bt_manager_is_connected() ? 1 : 0;
}

int akira_native_ble_char_write(wasm_exec_env_t exec_env,
				int32_t char_h,
				uint32_t data_ptr, uint32_t len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	if (len == 0) {
		return -EINVAL;
	}

	const uint8_t *data = (const uint8_t *)wasm_ptr_to_native(
		exec_env, data_ptr, len);

	if (!data) {
		LOG_ERR("ble_char_write: invalid WASM pointer 0x%x len=%u",
			data_ptr, len);
		return -EFAULT;
	}

	return ble_app_char_write((int)char_h, data, (uint16_t)len);
}

int akira_native_ble_char_read(wasm_exec_env_t exec_env,
			       int32_t char_h,
			       uint32_t buf_ptr, uint32_t len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	if (len == 0) {
		return -EINVAL;
	}

	uint8_t *buf = (uint8_t *)wasm_ptr_to_native(exec_env, buf_ptr, len);

	if (!buf) {
		LOG_ERR("ble_char_read: invalid WASM pointer");
		return -EFAULT;
	}

	return ble_app_char_read((int)char_h, buf, (uint16_t)len);
}

int akira_native_ble_event_pop(wasm_exec_env_t exec_env,
			       uint32_t buf_ptr, uint32_t len)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_BLE, -EACCES);

	/* Minimum buffer: 1 (type) + 1 (char_h) + 2 (data_len) = 4 bytes */
	if (len < 4) {
		return -EINVAL;
	}

	uint8_t *buf = (uint8_t *)wasm_ptr_to_native(exec_env, buf_ptr, len);

	if (!buf) {
		return -EFAULT;
	}

	struct ble_event evt;
	int evt_type = ble_app_event_pop(&evt);

	if (evt_type <= BLE_EVT_NONE) {
		return evt_type; /* 0 = empty, -errno = error */
	}

	/* Serialise event into WASM buffer:
	 *   [0]   type  (uint8)
	 *   [1]   char_handle (uint8)
	 *   [2-3] data_len LE (uint16)
	 *   [4..] data bytes
	 */
	buf[0] = evt.type;
	buf[1] = evt.char_handle;
	buf[2] = (uint8_t)(evt.data_len & 0xFF);
	buf[3] = (uint8_t)(evt.data_len >> 8);

	if (evt.data_len > 0) {
		uint16_t copy = (uint16_t)(len - 4);

		if (evt.data_len < copy) {
			copy = evt.data_len;
		}
		memcpy(buf + 4, evt.data, copy);
	}

	return evt_type;
}

#else /* !CONFIG_AKIRA_WASM_BLE */

/* Stubs so the linker does not complain if the header is included elsewhere */
int akira_native_ble_init(wasm_exec_env_t e)
{ (void)e; return -ENOTSUP; }
int akira_native_ble_deinit(wasm_exec_env_t e)
{ (void)e; return -ENOTSUP; }
int akira_native_ble_set_local_name(wasm_exec_env_t e, const char *n)
{ (void)e; (void)n; return -ENOTSUP; }
int akira_native_ble_service_create(wasm_exec_env_t e, const char *u)
{ (void)e; (void)u; return -ENOTSUP; }
int akira_native_ble_char_create(wasm_exec_env_t e, const char *u,
				 int32_t p, int32_t m)
{ (void)e; (void)u; (void)p; (void)m; return -ENOTSUP; }
int akira_native_ble_service_add_char(wasm_exec_env_t e, int32_t s, int32_t c)
{ (void)e; (void)s; (void)c; return -ENOTSUP; }
int akira_native_ble_add_service(wasm_exec_env_t e, int32_t s)
{ (void)e; (void)s; return -ENOTSUP; }
int akira_native_ble_set_advertised_service(wasm_exec_env_t e, int32_t s)
{ (void)e; (void)s; return -ENOTSUP; }
int akira_native_ble_advertise(wasm_exec_env_t e)    { (void)e; return -ENOTSUP; }
int akira_native_ble_stop_advertise(wasm_exec_env_t e) { (void)e; return -ENOTSUP; }
int akira_native_ble_is_connected(wasm_exec_env_t e) { (void)e; return -ENOTSUP; }
int akira_native_ble_char_write(wasm_exec_env_t e, int32_t h,
				uint32_t p, uint32_t l)
{ (void)e; (void)h; (void)p; (void)l; return -ENOTSUP; }
int akira_native_ble_char_read(wasm_exec_env_t e, int32_t h,
			       uint32_t p, uint32_t l)
{ (void)e; (void)h; (void)p; (void)l; return -ENOTSUP; }
int akira_native_ble_event_pop(wasm_exec_env_t e, uint32_t p, uint32_t l)
{ (void)e; (void)p; (void)l; return -ENOTSUP; }

#endif /* CONFIG_AKIRA_WASM_BLE */
