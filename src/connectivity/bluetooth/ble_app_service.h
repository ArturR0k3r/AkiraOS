/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_BLE_APP_SERVICE_H
#define AKIRA_BLE_APP_SERVICE_H

/**
 * @file ble_app_service.h
 * @brief Dynamic GATT service layer for WASM BLE apps.
 *
 * Provides a fixed-size pool of service and characteristic slots that
 * WASM apps allocate, configure, and register at runtime.  Only one
 * WASM app may hold the BLE_APP mode lock at a time.
 *
 * Typical flow:
 *   1. ble_app_svc_init()
 *   2. svc_h  = ble_app_svc_alloc("<UUID>")
 *   3. char_h = ble_app_char_alloc("<UUID>", BLE_PROP_READ|BLE_PROP_WRITE, 20)
 *   4. ble_app_svc_add_char(svc_h, char_h)
 *   5. ble_app_svc_register(svc_h)
 *   6. ble_app_char_write(char_h, data, len)   // notify peer
 *   7. ble_app_event_pop(evt)                  // drain event queue
 *   8. ble_app_svc_deinit()
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* BLE characteristic property flags (match Arduino BLE + BT spec)          */
/*===========================================================================*/

#define BLE_PROP_BROADCAST   0x01
#define BLE_PROP_READ        0x02
#define BLE_PROP_WRITE_WO_RSP 0x04
#define BLE_PROP_WRITE       0x08
#define BLE_PROP_NOTIFY      0x10
#define BLE_PROP_INDICATE    0x20

/*===========================================================================*/
/* BLE event types returned by ble_app_event_pop()                          */
/*===========================================================================*/

#define BLE_EVT_NONE         0
#define BLE_EVT_CONNECTED    1
#define BLE_EVT_DISCONNECTED 2
#define BLE_EVT_CHAR_WRITTEN 3

/**
 * @brief BLE event descriptor.
 *
 * Written into caller-provided memory by ble_app_event_pop().
 * data_len bytes of payload follow (relevant only for CHAR_WRITTEN).
 */
struct ble_event {
	uint8_t  type;                              /**< BLE_EVT_* */
	uint8_t  char_handle;                       /**< Characteristic slot index */
	uint16_t data_len;                          /**< Payload bytes (0 for connect events) */
	uint8_t  data[CONFIG_AKIRA_BLE_CHAR_MAX_LEN]; /**< Written value */
};

/*===========================================================================*/
/* Service / characteristic pool API                                         */
/*===========================================================================*/

/**
 * @brief Initialise the BLE app service subsystem.
 * @return 0 on success, negative errno otherwise.
 */
int ble_app_svc_init(void);

/**
 * @brief Release all GATT services and reset all slots.
 * @return 0 on success.
 */
int ble_app_svc_deinit(void);

/**
 * @brief Allocate a service slot with the given 128-bit UUID string.
 * @param uuid128_str  "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" format.
 * @return Service handle (>=0) on success, negative errno on failure.
 */
int ble_app_svc_alloc(const char *uuid128_str);

/**
 * @brief Allocate a characteristic slot.
 * @param uuid128_str  128-bit UUID string.
 * @param props        OR of BLE_PROP_* flags.
 * @param max_len      Maximum value length in bytes.
 * @return Characteristic handle (>=0) on success, negative errno on failure.
 */
int ble_app_char_alloc(const char *uuid128_str, uint8_t props, uint16_t max_len);

/**
 * @brief Add a characteristic to a service (before registration).
 * @return 0 on success, -EINVAL for bad handles, -ENOSPC if service is full.
 */
int ble_app_svc_add_char(int svc_h, int char_h);

/**
 * @brief Register a service with the GATT server.
 *
 * Must be called after all characteristics have been added and before
 * the app starts advertising.
 *
 * @return 0 on success, -ENOTSUP if already registered, negative errno on failure.
 */
int ble_app_svc_register(int svc_h);

/**
 * @brief Unregister all services and reset all slots.
 * @return 0 on success.
 */
int ble_app_svc_unregister_all(void);

/**
 * @brief Write a value to a characteristic and optionally notify connected peers.
 *
 * If the characteristic has BLE_PROP_NOTIFY set and a peer has subscribed
 * (written 0x0001 to the CCC descriptor), a notification is sent.
 *
 * @param char_h  Characteristic handle.
 * @param data    Data to write.
 * @param len     Number of bytes (must be <= max_len allocated for this char).
 * @return 0 on success, negative errno on failure.
 */
int ble_app_char_write(int char_h, const uint8_t *data, uint16_t len);

/**
 * @brief Read the current value of a characteristic.
 *
 * Returns the last value written by the local device (ble_app_char_write)
 * or by a connected peer (received via GATT write).
 *
 * @param char_h   Characteristic handle.
 * @param buf      Output buffer.
 * @param buf_len  Buffer capacity.
 * @return Bytes copied on success, negative errno on failure.
 */
int ble_app_char_read(int char_h, uint8_t *buf, uint16_t buf_len);

/**
 * @brief Pop the next BLE event from the queue (non-blocking).
 *
 * @param evt  Output event descriptor (caller allocates).
 * @return BLE_EVT_* type (>0) if an event was available, 0 if queue empty,
 *         negative errno on error.
 */
int ble_app_event_pop(struct ble_event *evt);

/**
 * @brief Push a connect/disconnect event from the BT manager callbacks.
 *
 * Called internally by bt_manager connection callbacks when mode is BLE_APP.
 *
 * @param type  BLE_EVT_CONNECTED or BLE_EVT_DISCONNECTED.
 */
void ble_app_push_conn_event(uint8_t type);

/**
 * @brief Return the 16-byte little-endian UUID of a registered service.
 *
 * @param svc_h  Service handle.
 * @return Pointer to 16-byte UUID, NULL if invalid handle.
 */
const uint8_t *ble_app_svc_get_uuid128(int svc_h);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BLE_APP_SERVICE_H */
