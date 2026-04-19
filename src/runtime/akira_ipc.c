/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_ipc
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ipc, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_ipc.c
 * @brief Inter-App Messaging implementation — named topic pub/sub
 *
 * Data model
 * ----------
 *  g_ipc_topics[]  — static array of topics (CONFIG_AKIRA_IPC_MAX_TOPICS)
 *    └── subs[]    — per-topic subscriber slots (CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC)
 *          └── msgq + msg_buf  — k_msgq + PSRAM-backed buffer per subscriber
 *
 * Each message enveloped in akira_ipc_msg_t: [uint16_t len][uint8_t data[MSG_MAX]].
 * Publish is non-blocking (k_msgq_put with K_NO_WAIT) — full queues drop silently.
 * Receive blocks according to the caller-supplied timeout.
 */

#include "akira_ipc.h"
#include <lib/mem_helper.h>  /* akira_malloc_buffer + AKIRA_BULK_BSS */
#include <string.h>
#include <errno.h>

/*===========================================================================*/
/* Internal Types                                                            */
/*===========================================================================*/

/** One subscriber slot inside a topic */
typedef struct {
    char           app_name[AKIRA_IPC_APP_NAME_MAX];
    struct k_msgq  msgq;
    uint8_t       *msg_buf; /**< PSRAM-allocated backing buffer for msgq */
    bool           used;
} akira_ipc_sub_t;

/** One topic entry */
typedef struct {
    char            name[AKIRA_IPC_TOPIC_NAME_MAX];
    akira_ipc_sub_t subs[CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC];
    bool            used;
} akira_ipc_topic_t;

/*===========================================================================*/
/* Global State                                                              */
/*===========================================================================*/

/* On PSRAM boards this lands in external RAM; otherwise in internal DRAM.
 * Keep CONFIG_AKIRA_IPC_MAX_TOPICS/SUBS small (defaults 4/2) on non-PSRAM boards. */
static akira_ipc_topic_t AKIRA_BULK_BSS g_ipc_topics[CONFIG_AKIRA_IPC_MAX_TOPICS];
static K_MUTEX_DEFINE(g_ipc_mutex);

/*===========================================================================*/
/* Internal Helpers                                                          */
/*===========================================================================*/

/** Find an existing topic by name (call with mutex held) */
static akira_ipc_topic_t *find_topic(const char *name)
{
    for (int i = 0; i < CONFIG_AKIRA_IPC_MAX_TOPICS; i++) {
        if (g_ipc_topics[i].used &&
            strncmp(g_ipc_topics[i].name, name,
                    AKIRA_IPC_TOPIC_NAME_MAX) == 0) {
            return &g_ipc_topics[i];
        }
    }
    return NULL;
}

/** Find-or-create a topic (call with mutex held) */
static akira_ipc_topic_t *get_or_create_topic(const char *name)
{
    akira_ipc_topic_t *t = find_topic(name);
    if (t) {
        return t;
    }

    for (int i = 0; i < CONFIG_AKIRA_IPC_MAX_TOPICS; i++) {
        if (!g_ipc_topics[i].used) {
            strncpy(g_ipc_topics[i].name, name,
                    AKIRA_IPC_TOPIC_NAME_MAX - 1);
            g_ipc_topics[i].name[AKIRA_IPC_TOPIC_NAME_MAX - 1] = '\0';
            g_ipc_topics[i].used = true;
            LOG_DBG("IPC topic created: '%s'", name);
            return &g_ipc_topics[i];
        }
    }
    return NULL; /* table full */
}

/** Find a subscription inside a topic (call with mutex held) */
static akira_ipc_sub_t *find_sub(akira_ipc_topic_t *topic,
                                  const char *app_name)
{
    for (int i = 0; i < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; i++) {
        if (topic->subs[i].used &&
            strncmp(topic->subs[i].app_name, app_name,
                    AKIRA_IPC_APP_NAME_MAX) == 0) {
            return &topic->subs[i];
        }
    }
    return NULL;
}

/** Allocate and initialise a new subscriber slot (call with mutex held) */
static akira_ipc_sub_t *create_sub(akira_ipc_topic_t *topic,
                                    const char *app_name)
{
    for (int i = 0; i < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; i++) {
        if (!topic->subs[i].used) {
            akira_ipc_sub_t *s = &topic->subs[i];

            /* Allocate PSRAM-backed buffer for the message queue */
            size_t buf_size = sizeof(akira_ipc_msg_t) *
                              CONFIG_AKIRA_IPC_QUEUE_DEPTH;
            s->msg_buf = (uint8_t *)akira_malloc_buffer(buf_size);
            if (!s->msg_buf) {
                LOG_ERR("IPC: failed to alloc msgq buffer (%zu bytes)", buf_size);
                return NULL;
            }

            k_msgq_init(&s->msgq, s->msg_buf, sizeof(akira_ipc_msg_t),
                        CONFIG_AKIRA_IPC_QUEUE_DEPTH);

            strncpy(s->app_name, app_name, AKIRA_IPC_APP_NAME_MAX - 1);
            s->app_name[AKIRA_IPC_APP_NAME_MAX - 1] = '\0';
            s->used = true;

            LOG_DBG("IPC: '%s' subscribed to '%s'",
                    app_name, topic->name);
            return s;
        }
    }
    return NULL; /* topic's sub table full */
}

/** Tear down a subscriber slot and free its buffer (call with mutex held) */
static void destroy_sub(akira_ipc_sub_t *s)
{
    k_msgq_purge(&s->msgq);
    akira_free_buffer(s->msg_buf);
    s->msg_buf = NULL;
    s->used    = false;
    s->app_name[0] = '\0';
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int akira_ipc_subscribe(const char *topic_name, const char *app_name)
{
    if (!topic_name || !app_name ||
        topic_name[0] == '\0' || app_name[0] == '\0') {
        return -EINVAL;
    }

    k_mutex_lock(&g_ipc_mutex, K_FOREVER);

    akira_ipc_topic_t *topic = get_or_create_topic(topic_name);
    if (!topic) {
        k_mutex_unlock(&g_ipc_mutex);
        LOG_ERR("IPC: topic table full (max %d)", CONFIG_AKIRA_IPC_MAX_TOPICS);
        return -ENOMEM;
    }

    /* Idempotent — already subscribed */
    if (find_sub(topic, app_name)) {
        k_mutex_unlock(&g_ipc_mutex);
        return 0;
    }

    akira_ipc_sub_t *sub = create_sub(topic, app_name);
    k_mutex_unlock(&g_ipc_mutex);

    if (!sub) {
        LOG_ERR("IPC: sub table full for topic '%s'", topic_name);
        return -ENOMEM;
    }
    return 0;
}

int akira_ipc_unsubscribe(const char *topic_name, const char *app_name)
{
    if (!topic_name || !app_name) {
        return -EINVAL;
    }

    k_mutex_lock(&g_ipc_mutex, K_FOREVER);

    akira_ipc_topic_t *topic = find_topic(topic_name);
    if (!topic) {
        k_mutex_unlock(&g_ipc_mutex);
        return -ENOENT;
    }

    akira_ipc_sub_t *sub = find_sub(topic, app_name);
    if (!sub) {
        k_mutex_unlock(&g_ipc_mutex);
        return -ENOENT;
    }

    destroy_sub(sub);

    /* If no more subscribers, free the topic slot */
    bool any_used = false;
    for (int i = 0; i < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; i++) {
        if (topic->subs[i].used) {
            any_used = true;
            break;
        }
    }
    if (!any_used) {
        topic->used     = false;
        topic->name[0]  = '\0';
        LOG_DBG("IPC: topic '%s' removed (no subscribers)", topic_name);
    }

    k_mutex_unlock(&g_ipc_mutex);
    return 0;
}

int akira_ipc_publish(const char *topic_name, const void *data, size_t len)
{
    if (!topic_name || !data) {
        return -EINVAL;
    }
    if (len == 0 || len > CONFIG_AKIRA_IPC_MSG_MAX_SIZE) {
        return -EMSGSIZE;
    }

    k_mutex_lock(&g_ipc_mutex, K_FOREVER);

    akira_ipc_topic_t *topic = find_topic(topic_name);
    if (!topic) {
        k_mutex_unlock(&g_ipc_mutex);
        /* No subscribers yet — not an error, message is silently discarded */
        return 0;
    }

    akira_ipc_msg_t msg;
    msg.len = (uint16_t)len;
    memcpy(msg.data, data, len);

    int delivered = 0;
    for (int i = 0; i < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; i++) {
        akira_ipc_sub_t *s = &topic->subs[i];
        if (!s->used) {
            continue;
        }
        int rc = k_msgq_put(&s->msgq, &msg, K_NO_WAIT);
        if (rc == 0) {
            delivered++;
        } else {
            LOG_DBG("IPC publish: queue full for sub '%s' on '%s', dropped",
                    s->app_name, topic_name);
        }
    }

    k_mutex_unlock(&g_ipc_mutex);
    return delivered;
}

int akira_ipc_recv(const char *topic_name, const char *app_name,
                   void *buf, size_t len, k_timeout_t timeout)
{
    if (!topic_name || !app_name || !buf || len == 0) {
        return -EINVAL;
    }

    /* Take a snapshot of the msgq pointer under the mutex, then receive
     * outside the lock so we don't hold it while blocking. */
    k_mutex_lock(&g_ipc_mutex, K_FOREVER);

    akira_ipc_topic_t *topic = find_topic(topic_name);
    if (!topic) {
        k_mutex_unlock(&g_ipc_mutex);
        return -ENOENT;
    }

    akira_ipc_sub_t *sub = find_sub(topic, app_name);
    if (!sub) {
        k_mutex_unlock(&g_ipc_mutex);
        return -ENOENT;
    }

    struct k_msgq *q = &sub->msgq;
    k_mutex_unlock(&g_ipc_mutex);

    akira_ipc_msg_t msg;
    int rc = k_msgq_get(q, &msg, timeout);
    if (rc != 0) {
        return -EAGAIN;
    }

    size_t copy = (msg.len < len) ? msg.len : len;
    memcpy(buf, msg.data, copy);
    return (int)copy;
}

int akira_ipc_pending(const char *topic_name, const char *app_name)
{
    if (!topic_name || !app_name) {
        return -EINVAL;
    }

    k_mutex_lock(&g_ipc_mutex, K_FOREVER);

    akira_ipc_topic_t *topic = find_topic(topic_name);
    if (!topic) {
        k_mutex_unlock(&g_ipc_mutex);
        return -ENOENT;
    }

    akira_ipc_sub_t *sub = find_sub(topic, app_name);
    if (!sub) {
        k_mutex_unlock(&g_ipc_mutex);
        return -ENOENT;
    }

    int pending = (int)k_msgq_num_used_get(&sub->msgq);
    k_mutex_unlock(&g_ipc_mutex);
    return pending;
}

void akira_ipc_cleanup_app(const char *app_name)
{
    if (!app_name || app_name[0] == '\0') {
        return;
    }

    k_mutex_lock(&g_ipc_mutex, K_FOREVER);

    for (int t = 0; t < CONFIG_AKIRA_IPC_MAX_TOPICS; t++) {
        akira_ipc_topic_t *topic = &g_ipc_topics[t];
        if (!topic->used) {
            continue;
        }

        for (int s = 0; s < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; s++) {
            akira_ipc_sub_t *sub = &topic->subs[s];
            if (sub->used &&
                strncmp(sub->app_name, app_name,
                        AKIRA_IPC_APP_NAME_MAX) == 0) {
                LOG_DBG("IPC cleanup: removing sub '%s' from topic '%s'",
                        app_name, topic->name);
                destroy_sub(sub);
            }
        }

        /* Free empty topic slots */
        bool any = false;
        for (int s = 0; s < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; s++) {
            if (topic->subs[s].used) {
                any = true;
                break;
            }
        }
        if (!any) {
            topic->used    = false;
            topic->name[0] = '\0';
        }
    }

    k_mutex_unlock(&g_ipc_mutex);
}
