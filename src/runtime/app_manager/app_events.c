/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_app_events
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_app_events, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file app_events.c
 * @brief App lifecycle event bus implementation.
 *
 * Thread-safety: handler table is protected by g_events_mutex.
 * app_events_publish() snapshots the active handler list before invoking
 * callbacks, so handlers may safely register/unregister inside a callback
 * without deadlocking.
 *
 * Important: handlers must NOT block or re-enter publish(). They are called
 * sequentially from the publishing thread, which may hold g_registry_mutex in
 * app_manager.c. Keep handler work minimal.
 */

#include "app_events.h"
#include <zephyr/kernel.h>
#include <string.h>
#include <stdint.h>

#define MAX_EVENT_HANDLERS 4

typedef struct {
    app_event_handler_t fn;
    void *user_data;
    bool used;
} handler_entry_t;

static handler_entry_t g_handlers[MAX_EVENT_HANDLERS];
static K_MUTEX_DEFINE(g_events_mutex);
static bool g_initialized = false;

int app_events_init(void)
{
    k_mutex_lock(&g_events_mutex, K_FOREVER);
    memset(g_handlers, 0, sizeof(g_handlers));
    g_initialized = true;
    k_mutex_unlock(&g_events_mutex);
    LOG_DBG("App event bus initialized (max handlers: %d)", MAX_EVENT_HANDLERS);
    return 0;
}

int app_events_register_handler(app_event_handler_t handler, void *user_data)
{
    if (!handler) {
        return -EINVAL;
    }

    k_mutex_lock(&g_events_mutex, K_FOREVER);
    for (int i = 0; i < MAX_EVENT_HANDLERS; i++) {
        if (!g_handlers[i].used) {
            g_handlers[i].fn        = handler;
            g_handlers[i].user_data = user_data;
            g_handlers[i].used      = true;
            k_mutex_unlock(&g_events_mutex);
            LOG_DBG("Event handler registered at slot %d", i);
            return i;
        }
    }
    k_mutex_unlock(&g_events_mutex);
    LOG_WRN("No free handler slots (max %d)", MAX_EVENT_HANDLERS);
    return -ENOMEM;
}

int app_events_unregister_handler(int handler_id)
{
    if (handler_id < 0 || handler_id >= MAX_EVENT_HANDLERS) {
        return -EINVAL;
    }

    k_mutex_lock(&g_events_mutex, K_FOREVER);
    g_handlers[handler_id].fn        = NULL;
    g_handlers[handler_id].user_data = NULL;
    g_handlers[handler_id].used      = false;
    k_mutex_unlock(&g_events_mutex);
    return 0;
}

int app_events_publish(const app_event_t *event)
{
    if (!event || !g_initialized) {
        return -EINVAL;
    }

    /* Snapshot the handler table so we can release the mutex before invoking
     * callbacks.  This allows handlers to register/unregister without
     * deadlocking and keeps the critical section short. */
    handler_entry_t snapshot[MAX_EVENT_HANDLERS];

    k_mutex_lock(&g_events_mutex, K_FOREVER);
    memcpy(snapshot, g_handlers, sizeof(snapshot));
    k_mutex_unlock(&g_events_mutex);

    for (int i = 0; i < MAX_EVENT_HANDLERS; i++) {
        if (snapshot[i].used && snapshot[i].fn) {
            snapshot[i].fn(event, snapshot[i].user_data);
        }
    }
    return 0;
}

int app_events_publish_simple(app_event_type_t type, int container_id,
                               const char *app_name)
{
    app_event_t event = {0};

    event.type         = type;
    event.container_id = container_id;
    event.timestamp_ms = k_uptime_get_32();

    if (app_name) {
        strncpy(event.app_name, app_name, sizeof(event.app_name) - 1);
    }

    return app_events_publish(&event);
}
