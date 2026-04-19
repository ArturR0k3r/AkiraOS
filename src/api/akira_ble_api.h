/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_BLE_API_H
#define AKIRA_BLE_API_H

/**
 * @file akira_ble_api.h
 * @brief Arduino-style BLE native API exposed to WASM apps.
 *
 * Required manifest capability: "ble"
 *
 * Typical WASM app flow (mirrors Arduino BLE usage):
 *
 *   int svc  = ble_service_create("19B10000-E8F2-537E-4F6C-D104768A1214");
 *   int ch   = ble_char_create("19B10001-...", BLE_PROP_READ | BLE_PROP_WRITE, 1);
 *   ble_service_add_char(svc, ch);
 *   ble_add_service(svc);
 *   ble_set_local_name("AkiraOS_LED");
 *   ble_set_advertised_service(svc);
 *   ble_init();
 *   ble_advertise();
 *
 *   while (1) {
 *       int evt = ble_event_pop(buf, sizeof(buf));
 *       if (evt == BLE_EVT_CHAR_WRITTEN) { ... }
 *   }
 */

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* WASM native export functions — one per BLE API symbol              */
/* All require AKIRA_CAP_BLE in the app manifest.                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise BLE in BLE_APP mode (lazy BT stack start).
 * Fails with -EBUSY if HID mode is already active.
 */
int akira_native_ble_init(wasm_exec_env_t exec_env);

/**
 * @brief Deinitialise BLE, unregister all services, release BLE_APP lock.
 */
int akira_native_ble_deinit(wasm_exec_env_t exec_env);

/**
 * @brief Set the BLE device name seen by scanning peers.
 * @param name  Null-terminated C string (max 29 bytes).
 */
int akira_native_ble_set_local_name(wasm_exec_env_t exec_env,
				    const char *name);

/**
 * @brief Create a GATT service with a 128-bit UUID.
 * @param uuid128_str  "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 * @return Service handle (>=0) or negative errno.
 */
int akira_native_ble_service_create(wasm_exec_env_t exec_env,
				    const char *uuid128_str);

/**
 * @brief Create a GATT characteristic.
 * @param uuid128_str  128-bit UUID string.
 * @param props        OR of BLE_PROP_* flags.
 * @param max_len      Maximum value length (bytes).
 * @return Characteristic handle (>=0) or negative errno.
 */
int akira_native_ble_char_create(wasm_exec_env_t exec_env,
				 const char *uuid128_str,
				 int32_t props,
				 int32_t max_len);

/**
 * @brief Add a characteristic to a service.
 * @param svc_h   Service handle.
 * @param char_h  Characteristic handle.
 */
int akira_native_ble_service_add_char(wasm_exec_env_t exec_env,
				      int32_t svc_h, int32_t char_h);

/**
 * @brief Finalise a service and register it with the GATT server.
 * Call this once all characteristics have been added.
 * @param svc_h  Service handle.
 */
int akira_native_ble_add_service(wasm_exec_env_t exec_env, int32_t svc_h);

/**
 * @brief Choose which service UUID is included in the advertisement/scan-response.
 * @param svc_h  Service handle to advertise.
 */
int akira_native_ble_set_advertised_service(wasm_exec_env_t exec_env,
					    int32_t svc_h);

/**
 * @brief Start BLE advertising with the previously configured service UUID.
 */
int akira_native_ble_advertise(wasm_exec_env_t exec_env);

/**
 * @brief Stop BLE advertising.
 */
int akira_native_ble_stop_advertise(wasm_exec_env_t exec_env);

/**
 * @brief Return 1 if a BLE peer is connected, 0 otherwise.
 */
int akira_native_ble_is_connected(wasm_exec_env_t exec_env);

/**
 * @brief Write (and optionally notify) a characteristic value.
 * @param char_h   Characteristic handle.
 * @param data_ptr WASM pointer to data buffer.
 * @param len      Number of bytes.
 */
int akira_native_ble_char_write(wasm_exec_env_t exec_env,
				int32_t char_h,
				uint32_t data_ptr, uint32_t len);

/**
 * @brief Read the current value of a characteristic.
 * @param char_h   Characteristic handle.
 * @param buf_ptr  WASM pointer to destination buffer.
 * @param len      Buffer capacity.
 * @return Bytes copied, or negative errno.
 */
int akira_native_ble_char_read(wasm_exec_env_t exec_env,
			       int32_t char_h,
			       uint32_t buf_ptr, uint32_t len);

/**
 * @brief Pop the next BLE event from the internal queue.
 *
 * On success the event is serialised into WASM memory at @p buf_ptr:
 *   Byte 0        : event type (BLE_EVT_* from akira_api.h)
 *   Byte 1        : char_handle
 *   Bytes 2-3 LE  : data_len
 *   Bytes 4+      : data payload (data_len bytes)
 *
 * @param buf_ptr  WASM pointer to output buffer.
 * @param len      Buffer capacity (must be >= 4 + max data).
 * @return Event type (>0) if available, 0 if queue empty, negative errno on error.
 */
int akira_native_ble_event_pop(wasm_exec_env_t exec_env,
			       uint32_t buf_ptr, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BLE_API_H */
