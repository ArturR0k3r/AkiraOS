/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_APP_CMD_BRIDGE_H
#define AKIRA_APP_CMD_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file app_cmd_bridge.h
 * @brief Bridges external transports (BLE companion, AkiraHub) to a running
 * app's IPC topics, so phone/hub can send an app-defined command and get an
 * app-defined reply back without any new WASM API.
 *
 * Fixed topic convention: "<appname>.cmd" (host publishes, app subscribes
 * itself with msg_subscribe()) and "<appname>.evt" (app publishes its reply,
 * this module subscribes on the app's behalf only while a request is
 * in flight).
 */

typedef enum {
    APP_CMD_REPLY_OK = 0,
    APP_CMD_REPLY_NOT_LISTENING = -1, /**< app not running or hasn't subscribed to its .cmd topic */
    APP_CMD_REPLY_TIMEOUT = -2,       /**< no reply within the given timeout */
} app_cmd_reply_status_t;

typedef void (*app_cmd_reply_cb_t)(const char *app_name, const void *reply, size_t reply_len,
                                   app_cmd_reply_status_t status, void *user_data);

/**
 * @brief Send an opaque command to a running app and wait for its reply.
 *
 * Blocking: waits up to @p timeout for the app's reply on "<app_name>.evt"
 * before invoking @p cb with APP_CMD_REPLY_TIMEOUT. Safe to call from a
 * dedicated workqueue context (BLE cmd_work, Hub cloud worker) — never call
 * from the BT thread or a WASM native call directly.
 *
 * @return 0 if the command was delivered (cb will be invoked, possibly with
 *         a timeout status), negative errno if the app isn't listening or
 *         the app isn't running.
 */
int app_cmd_bridge_send(const char *app_name, const void *payload, size_t len,
                        app_cmd_reply_cb_t cb, void *user_data, k_timeout_t timeout);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_APP_CMD_BRIDGE_H */
