/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "app_cmd_bridge.h"
#include <runtime/app_manager/app_manager.h>
#include <runtime/akira_ipc.h>

#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

LOG_MODULE_REGISTER(app_cmd_bridge, CONFIG_AKIRA_LOG_LEVEL);

#define BRIDGE_SUBSCRIBER_NAME "__bridge__"
#define EVT_TOPIC_SUFFIX ".evt"
#define CMD_TOPIC_SUFFIX ".cmd"
/* AKIRA_IPC_TOPIC_NAME_MAX (akira_ipc.h) bounds "<name>" + ".cmd"/".evt" */
#define TOPIC_NAME_BUF_LEN AKIRA_IPC_TOPIC_NAME_MAX

int app_cmd_bridge_send(const char *app_name, const void *payload, size_t len,
                        app_cmd_reply_cb_t cb, void *user_data, k_timeout_t timeout)
{
    if (!app_name || !app_name[0] || !cb) {
        return -EINVAL;
    }

    if (app_manager_get_state(app_name) != APP_STATE_RUNNING) {
        cb(app_name, NULL, 0, APP_CMD_REPLY_NOT_LISTENING, user_data);
        return -ENOENT;
    }

    char cmd_topic[TOPIC_NAME_BUF_LEN];
    char evt_topic[TOPIC_NAME_BUF_LEN];
    snprintf(cmd_topic, sizeof(cmd_topic), "%s" CMD_TOPIC_SUFFIX, app_name);
    snprintf(evt_topic, sizeof(evt_topic), "%s" EVT_TOPIC_SUFFIX, app_name);

    /* Subscribe to the reply topic BEFORE publishing the command — otherwise
     * a fast-replying app can publish to evt_topic while nobody is subscribed
     * yet, dropping the reply and stalling this call until timeout. */
    int rc = akira_ipc_subscribe(evt_topic, BRIDGE_SUBSCRIBER_NAME);
    if (rc < 0 && rc != -EALREADY) {
        LOG_ERR("app_cmd_bridge: subscribe to %s failed: %d", evt_topic, rc);
        cb(app_name, NULL, 0, APP_CMD_REPLY_TIMEOUT, user_data);
        return rc;
    }

    /* akira_ipc_publish()'s return is the subscriber count that received the
     * message (akira_ipc.h doc) — 0 means the app hasn't subscribed to its
     * own .cmd topic yet, i.e. "not listening". No separate query needed. */
    int delivered = akira_ipc_publish(cmd_topic, payload, len);
    if (delivered <= 0) {
        akira_ipc_unsubscribe(evt_topic, BRIDGE_SUBSCRIBER_NAME);
        cb(app_name, NULL, 0, APP_CMD_REPLY_NOT_LISTENING, user_data);
        return -ENOENT;
    }

    uint8_t reply_buf[CONFIG_AKIRA_IPC_MSG_MAX_SIZE];
    int reply_len = akira_ipc_recv(evt_topic, BRIDGE_SUBSCRIBER_NAME,
                                   reply_buf, sizeof(reply_buf), timeout);

    akira_ipc_unsubscribe(evt_topic, BRIDGE_SUBSCRIBER_NAME);

    if (reply_len < 0) {
        cb(app_name, NULL, 0, APP_CMD_REPLY_TIMEOUT, user_data);
        return 0; /* delivered fine; reply just never came — not a send error */
    }

    cb(app_name, reply_buf, (size_t)reply_len, APP_CMD_REPLY_OK, user_data);
    return 0;
}
