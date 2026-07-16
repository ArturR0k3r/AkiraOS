/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_COMPANION_SERVICE_H
#define AKIRA_COMPANION_SERVICE_H

/**
 * @file companion_service.h
 * @brief BLE Companion GATT service for the AkiraApp mobile application.
 *
 * Provides a five-characteristic GATT service that the AkiraApp phone
 * application (React Native / Flutter, iOS + Android) uses to manage an
 * AkiraConsole device over Bluetooth LE.
 *
 * Service UUID: A1524C02-0001-4E56-8D4E-494B52413001
 *
 * Characteristics:
 *
 *  Name       | UUID …3001 suffix | Properties           | Max size
 *  -----------+-------------------+----------------------+---------
 *  CMD_CHAR   | 0002              | WRITE                | 244 B
 *  RESP_CHAR  | 0003              | NOTIFY               | 244 B
 *  DATA_UP    | 0004              | WRITE_WITHOUT_RSP    | 244 B
 *  DATA_DOWN  | 0005              | NOTIFY               | 244 B
 *  STATUS     | 0006              | READ + NOTIFY        | 244 B
 *
 * Protocol summary:
 *
 *  Phone writes a JSON command object to CMD_CHAR:
 *    {"op":"<operation>","id":<int>,"params":{...}}
 *
 *  Device responds via RESP_CHAR notification:
 *    {"op":"<operation>","id":<int>,"ok":true,"data":{...}}
 *    {"op":"<operation>","id":<int>,"ok":false,"error":"<message>"}
 *
 *  Bulk transfers (app install, file read/write) stream chunks over
 *  DATA_UP (phone→device) and DATA_DOWN (device→phone) using a simple
 *  header protocol described in companion_service.c.
 *
 *  STATUS_CHAR is notified every CONFIG_AKIRA_BT_COMPANION_STATUS_INTERVAL_MS
 *  and contains the same JSON object as the cloud device.status message.
 *
 * Enabled by CONFIG_AKIRA_BT_COMPANION.
 * See docs/hub/PHONE_APP_INTEGRATION.md for the full integration spec.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Service / characteristic UUIDs (base: A1524C02-xxxx-4E56-8D4E-494B52413001)
 * -------------------------------------------------------------------------- */

/** 128-bit service UUID (little-endian) */
#define COMPANION_SVC_UUID \
    BT_UUID_128_ENCODE(0xA1524C02, 0x0001, 0x4E56, 0x8D4E, 0x494B52413001)

/** CMD_CHAR  — phone → device JSON commands */
#define COMPANION_CMD_UUID \
    BT_UUID_128_ENCODE(0xA1524C02, 0x0002, 0x4E56, 0x8D4E, 0x494B52413001)

/** RESP_CHAR — device → phone JSON responses (notify) */
#define COMPANION_RESP_UUID \
    BT_UUID_128_ENCODE(0xA1524C02, 0x0003, 0x4E56, 0x8D4E, 0x494B52413001)

/** DATA_UP   — phone → device bulk data (write without response) */
#define COMPANION_DATA_UP_UUID \
    BT_UUID_128_ENCODE(0xA1524C02, 0x0004, 0x4E56, 0x8D4E, 0x494B52413001)

/** DATA_DOWN — device → phone bulk data (notify) */
#define COMPANION_DATA_DOWN_UUID \
    BT_UUID_128_ENCODE(0xA1524C02, 0x0005, 0x4E56, 0x8D4E, 0x494B52413001)

/** STATUS    — device status snapshot (read + notify) */
#define COMPANION_STATUS_UUID \
    BT_UUID_128_ENCODE(0xA1524C02, 0x0006, 0x4E56, 0x8D4E, 0x494B52413001)

/* --------------------------------------------------------------------------
 * Command operation strings (written to CMD_CHAR)
 * -------------------------------------------------------------------------- */

/**
 * @brief All operation strings accepted on CMD_CHAR.
 *
 * Each op maps to a handler in companion_service.c.  Parameters sent
 * in the "params" object are documented in docs/hub/PHONE_APP_INTEGRATION.md.
 */
#define COMP_OP_DEVICE_INFO       "device.info"
#define COMP_OP_DEVICE_REBOOT     "device.reboot"

#define COMP_OP_APPS_LIST         "apps.list"
#define COMP_OP_APPS_START        "apps.start"         /* params: {"name":"..."} */
#define COMP_OP_APPS_STOP         "apps.stop"          /* params: {"name":"..."} */
#define COMP_OP_APPS_UNINSTALL    "apps.uninstall"     /* params: {"name":"..."} */
#define COMP_OP_APPS_INSTALL_BEGIN "apps.install.begin" /* params: {"name":"...","size":<bytes>,"crc32":<uint>} */
#define COMP_OP_APPS_INSTALL_END  "apps.install.end"   /* no params — commit install */

#define COMP_OP_SETTINGS_GET      "settings.get"       /* params: {"key":"..."} */
#define COMP_OP_SETTINGS_SET      "settings.set"       /* params: {"key":"...","value":"..."} */
#define COMP_OP_SETTINGS_LIST     "settings.list"

#define COMP_OP_SHELL_EXEC        "shell.exec"         /* params: {"cmd":"..."} */

#define COMP_OP_FILES_LIST        "files.list"         /* params: {"path":"..."} */
#define COMP_OP_FILES_READ        "files.read"         /* params: {"path":"..."} — data streams on DATA_DOWN */
#define COMP_OP_FILES_WRITE       "files.write"        /* params: {"path":"...","size":<bytes>} — data via DATA_UP */
#define COMP_OP_FILES_DELETE      "files.delete"       /* params: {"path":"..."} */
#define COMP_OP_FILES_MKDIR       "files.mkdir"        /* params: {"path":"..."} */

#define COMP_OP_OTA_START         "ota.start"          /* params: {"url":"...","version":"...","signature":"<hex>"} */
#define COMP_OP_OTA_STATUS        "ota.status"

/* --------------------------------------------------------------------------
 * Data transfer framing (DATA_UP / DATA_DOWN)
 *
 * Each 244-byte ATT write over DATA_UP or DATA_DOWN begins with a 4-byte
 * header:
 *
 *   Byte 0:   Transfer type
 *               COMP_XFER_APP_DATA   0x01
 *               COMP_XFER_FILE_DATA  0x02
 *               COMP_XFER_SHELL_OUT  0x03  (data down only — shell stream)
 *   Byte 1:   Flags
 *               COMP_FLAG_LAST       0x01  set on final chunk
 *               COMP_FLAG_ERROR      0x02  set if sender aborted
 *   Byte 2-3: Payload length (LE uint16) — bytes that follow
 *   Byte 4+:  Payload
 * -------------------------------------------------------------------------- */

#define COMP_XFER_APP_DATA   0x01
#define COMP_XFER_FILE_DATA  0x02
#define COMP_XFER_SHELL_OUT  0x03

#define COMP_FLAG_LAST       0x01
#define COMP_FLAG_ERROR      0x02

/** Maximum payload per DATA_UP/DATA_DOWN frame (ATT MTU 247 - 3 L2CAP - 4 header) */
#define COMP_DATA_PAYLOAD_MAX 240

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialise and register the Companion GATT service.
 *
 * Registers the static GATT service table, initialises the command handler
 * work queue, and starts the periodic status timer.
 *
 * Must be called after bt_manager_init() and before advertising starts.
 * Typically invoked from SYS_INIT at APPLICATION level when
 * CONFIG_AKIRA_BT_COMPANION=y.
 *
 * @return 0 on success, negative errno on failure.
 */
int companion_svc_init(void);

/**
 * @brief Unregister the Companion GATT service.
 *
 * Cancels the status timer, drains any pending work, and unregisters the
 * GATT attributes.  The BT mode is reset to BT_MODE_NONE.
 *
 * @return 0 on success, -EALREADY if not initialised.
 */
int companion_svc_deinit(void);

/**
 * @brief Push an unsolicited device-status notification to a connected peer.
 *
 * Builds the status JSON payload and notifies STATUS_CHAR.  Called
 * periodically by the internal timer and may also be called directly when
 * significant state changes occur (e.g. app started/stopped).
 *
 * No-op if no peer has subscribed to STATUS_CHAR notifications or if no
 * peer is currently connected.
 */
void companion_svc_notify_status(void);

/**
 * @brief Check whether the companion service is initialised.
 *
 * @return true if companion_svc_init() has been called successfully.
 */
bool companion_svc_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_COMPANION_SERVICE_H */
