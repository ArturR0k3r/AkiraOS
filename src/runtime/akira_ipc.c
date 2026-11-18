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
 *
 * Subscribers are either WASM apps, identified by their runtime slot (the
 * *_slot functions, used by the native API), or system code identified by a
 * name (the plain functions, e.g. the remote command bridge). An app's
 * subscriptions are released by slot when it ends, however it ends, and a
 * stop wakes an app blocked in a receive.
 */

#include "akira_ipc.h"
#include <lib/mem_helper.h>  /* AKIRA_BULK_BSS */
#include <akira_app_containment.h>
#include <string.h>
#include <errno.h>

#ifdef CONFIG_AKIRA_PSRAM
#include <zephyr/multi_heap/shared_multi_heap.h>
#endif

/*===========================================================================*/
/* Internal Types                                                            */
/*===========================================================================*/

/** One subscriber slot inside a topic */
typedef struct {
    char           app_name[AKIRA_IPC_APP_NAME_MAX];
    struct k_msgq  msgq;
    uint8_t       *msg_buf;       /**< backing buffer for msgq (ipc_buf_alloc) */
    bool           msg_buf_psram; /**< msg_buf came from the PSRAM heap */
    int8_t         owner_slot;    /**< WASM app slot, or -1 for a system subscriber */
    bool           used;
} akira_ipc_sub_t;

/** One topic entry */
typedef struct {
    char            name[AKIRA_IPC_TOPIC_NAME_MAX];
    akira_ipc_sub_t subs[CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC];
    bool            used;
} akira_ipc_topic_t;

#define OWNER_SYSTEM (-1)

/* Envelope length that marks a stop wake-up, never a real message. */
#define IPC_WAKE_LEN 0xFFFFu

/* Every lock is bounded: if an app was ever aborted holding the mutex, IPC
 * calls fail with -EBUSY instead of hanging the app manager or HOME. Holds
 * are a few microseconds, so this never trips in normal operation. */
#define IPC_LOCK_TIMEOUT K_MSEC(1000)

/*===========================================================================*/
/* Global State                                                              */
/*===========================================================================*/

/* On PSRAM boards this lands in external RAM; otherwise in internal DRAM.
 * Keep CONFIG_AKIRA_IPC_MAX_TOPICS/SUBS small (defaults 4/2) on non-PSRAM boards. */
static akira_ipc_topic_t AKIRA_BULK_BSS g_ipc_topics[CONFIG_AKIRA_IPC_MAX_TOPICS];
static K_MUTEX_DEFINE(g_ipc_mutex);

/* Copied into a subscriber's queue to wake a receive when its app is stopped. */
static const akira_ipc_msg_t g_ipc_wake = { .len = IPC_WAKE_LEN };

/*===========================================================================*/
/* Internal Helpers                                                          */
/*===========================================================================*/

static int ipc_lock(void)
{
    return k_mutex_lock(&g_ipc_mutex, IPC_LOCK_TIMEOUT) == 0 ? 0 : -EBUSY;
}

static void ipc_unlock(void)
{
    k_mutex_unlock(&g_ipc_mutex);
}

/* strnlen() without relying on POSIX visibility. */
static size_t bounded_strlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* Names must fit their table field: a longer one would be truncated on
 * create and then never match again, leaking a topic per call. */
static bool name_ok(const char *name, size_t max)
{
    return name && name[0] != '\0' && bounded_strlen(name, max) < max;
}

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

/** Free a topic slot once it has no subscriber (call with mutex held) */
static void release_topic_if_empty(akira_ipc_topic_t *topic)
{
    for (int i = 0; i < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; i++) {
        if (topic->subs[i].used) {
            return;
        }
    }
    LOG_DBG("IPC: topic '%s' removed (no subscribers)", topic->name);
    topic->used    = false;
    topic->name[0] = '\0';
}

/** Find a subscription inside a topic (call with mutex held).
 *  Apps match by slot; system subscribers by name. */
static akira_ipc_sub_t *find_sub(akira_ipc_topic_t *topic, int owner_slot,
                                 const char *app_name)
{
    for (int i = 0; i < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; i++) {
        akira_ipc_sub_t *s = &topic->subs[i];

        if (!s->used || s->owner_slot != owner_slot) {
            continue;
        }
        if (owner_slot != OWNER_SYSTEM ||
            strncmp(s->app_name, app_name, AKIRA_IPC_APP_NAME_MAX) == 0) {
            return s;
        }
    }
    return NULL;
}

/** Subscriptions held by an app slot (call with mutex held) */
static int count_slot_subs(int owner_slot)
{
    int n = 0;

    for (int t = 0; t < CONFIG_AKIRA_IPC_MAX_TOPICS; t++) {
        if (!g_ipc_topics[t].used) {
            continue;
        }
        for (int s = 0; s < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; s++) {
            const akira_ipc_sub_t *sub = &g_ipc_topics[t].subs[s];

            if (sub->used && sub->owner_slot == owner_slot) {
                n++;
            }
        }
    }
    return n;
}

/* Queue backing buffers (CONFIG_AKIRA_IPC_QUEUE_DEPTH envelopes each).
 *
 * On PSRAM boards an app's queue comes from PSRAM only: it must never fall
 * back to the internal-DRAM system heap, which the BLE controller and Wi-Fi
 * need. System subscribers may fall back. Without PSRAM the system heap is
 * all there is; the per-app subscription cap bounds what an app can take. */
static uint8_t *ipc_buf_alloc(size_t size, bool app_owned, bool *psram)
{
    *psram = false;
#ifdef CONFIG_AKIRA_PSRAM
    uint8_t *p = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, size);

    if (p) {
        *psram = true;
        return p;
    }
    if (app_owned) {
        return NULL;
    }
#else
    ARG_UNUSED(app_owned);
#endif
    return k_malloc(size);
}

static void ipc_buf_free(uint8_t *p, bool psram)
{
    if (!p) {
        return;
    }
#ifdef CONFIG_AKIRA_PSRAM
    if (psram) {
        shared_multi_heap_free(p);
        return;
    }
#else
    ARG_UNUSED(psram);
#endif
    k_free(p);
}

/** Allocate and initialise a new subscriber slot (call with mutex held) */
static akira_ipc_sub_t *create_sub(akira_ipc_topic_t *topic, int owner_slot,
                                   const char *app_name)
{
    for (int i = 0; i < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; i++) {
        if (!topic->subs[i].used) {
            akira_ipc_sub_t *s = &topic->subs[i];

            size_t buf_size = sizeof(akira_ipc_msg_t) *
                              CONFIG_AKIRA_IPC_QUEUE_DEPTH;
            s->msg_buf = ipc_buf_alloc(buf_size, owner_slot != OWNER_SYSTEM,
                                       &s->msg_buf_psram);
            if (!s->msg_buf) {
                return NULL;
            }

            k_msgq_init(&s->msgq, s->msg_buf, sizeof(akira_ipc_msg_t),
                        CONFIG_AKIRA_IPC_QUEUE_DEPTH);

            strncpy(s->app_name, app_name ? app_name : "",
                    AKIRA_IPC_APP_NAME_MAX - 1);
            s->app_name[AKIRA_IPC_APP_NAME_MAX - 1] = '\0';
            s->owner_slot = (int8_t)owner_slot;
            s->used = true;

            LOG_DBG("IPC: '%s' (slot %d) subscribed to '%s'",
                    s->app_name, owner_slot, topic->name);
            return s;
        }
    }
    return NULL; /* topic's sub table full */
}

/** Tear down a subscriber slot and free its buffer (call with mutex held).
 *  Only ever called for a subscription whose owner is not blocked in it:
 *  the owner itself, or a release after the owner thread ended. */
static void destroy_sub(akira_ipc_sub_t *s)
{
    k_msgq_purge(&s->msgq);
    ipc_buf_free(s->msg_buf, s->msg_buf_psram);
    s->msg_buf = NULL;
    s->msg_buf_psram = false;
    s->used    = false;
    s->owner_slot = OWNER_SYSTEM;
    s->app_name[0] = '\0';
}

/*===========================================================================*/
/* Core (shared by the app and system entry points)                          */
/*===========================================================================*/

static int ipc_subscribe(const char *topic_name, int owner_slot, const char *app_name)
{
    if (!name_ok(topic_name, AKIRA_IPC_TOPIC_NAME_MAX)) {
        return -EINVAL;
    }
    if (ipc_lock() < 0) {
        return -EBUSY;
    }

    akira_ipc_topic_t *topic = find_topic(topic_name);

    /* Idempotent — already subscribed */
    if (topic && find_sub(topic, owner_slot, app_name)) {
        ipc_unlock();
        return 0;
    }

    if (owner_slot != OWNER_SYSTEM &&
        count_slot_subs(owner_slot) >= AKIRA_IPC_MAX_SUBS_PER_APP) {
        ipc_unlock();
        LOG_WRN_RATELIMIT_RATE(1000, "IPC: app slot %d already holds %d subscriptions",
                               owner_slot, AKIRA_IPC_MAX_SUBS_PER_APP);
        return -ENOMEM;
    }

    if (!topic) {
        topic = get_or_create_topic(topic_name);
    }
    if (!topic) {
        ipc_unlock();
        LOG_WRN_RATELIMIT_RATE(1000, "IPC: topic table full (max %d)",
                               CONFIG_AKIRA_IPC_MAX_TOPICS);
        return -ENOMEM;
    }

    akira_ipc_sub_t *sub = create_sub(topic, owner_slot, app_name);
    if (!sub) {
        /* Don't leave a topic created for this call behind with no
         * subscribers: it would hold a topic slot forever. */
        release_topic_if_empty(topic);
    }
    ipc_unlock();

    if (!sub) {
        LOG_WRN_RATELIMIT_RATE(1000, "IPC: no subscription for '%s' (table full or out of memory)",
                               topic_name);
        return -ENOMEM;
    }
    return 0;
}

static int ipc_unsubscribe(const char *topic_name, int owner_slot, const char *app_name)
{
    if (!name_ok(topic_name, AKIRA_IPC_TOPIC_NAME_MAX)) {
        return -EINVAL;
    }
    if (ipc_lock() < 0) {
        return -EBUSY;
    }

    akira_ipc_topic_t *topic = find_topic(topic_name);
    akira_ipc_sub_t *sub = topic ? find_sub(topic, owner_slot, app_name) : NULL;

    if (!sub) {
        ipc_unlock();
        return -ENOENT;
    }

    destroy_sub(sub);
    release_topic_if_empty(topic);
    ipc_unlock();
    return 0;
}

static int ipc_recv(const char *topic_name, int owner_slot, const char *app_name,
                    void *buf, size_t len, k_timeout_t timeout)
{
    if (!name_ok(topic_name, AKIRA_IPC_TOPIC_NAME_MAX) || !buf || len == 0) {
        return -EINVAL;
    }

    /* Take a snapshot of the msgq pointer under the mutex, then receive
     * outside the lock so we don't hold it while blocking. Only the owner
     * removes its own subscription, so it cannot vanish under this wait. */
    if (ipc_lock() < 0) {
        return -EBUSY;
    }

    akira_ipc_topic_t *topic = find_topic(topic_name);
    akira_ipc_sub_t *sub = topic ? find_sub(topic, owner_slot, app_name) : NULL;

    if (!sub) {
        ipc_unlock();
        return -ENOENT;
    }

    struct k_msgq *q = &sub->msgq;
    ipc_unlock();

    akira_ipc_msg_t msg;
    int rc = k_msgq_get(q, &msg, timeout);
    if (rc != 0) {
        return -EAGAIN;
    }
    if (msg.len == IPC_WAKE_LEN) {
        return -EINTR; /* the owning app is being stopped */
    }

    size_t copy = MIN((size_t)msg.len, MIN(len, sizeof(msg.data)));
    memcpy(buf, msg.data, copy);
    return (int)copy;
}

static int ipc_pending(const char *topic_name, int owner_slot, const char *app_name)
{
    if (!name_ok(topic_name, AKIRA_IPC_TOPIC_NAME_MAX)) {
        return -EINVAL;
    }
    if (ipc_lock() < 0) {
        return -EBUSY;
    }

    akira_ipc_topic_t *topic = find_topic(topic_name);
    akira_ipc_sub_t *sub = topic ? find_sub(topic, owner_slot, app_name) : NULL;

    if (!sub) {
        ipc_unlock();
        return -ENOENT;
    }

    int pending = (int)k_msgq_num_used_get(&sub->msgq);
    ipc_unlock();
    return pending;
}

/*===========================================================================*/
/* Public API — system subscribers (by name)                                 */
/*===========================================================================*/

int akira_ipc_subscribe(const char *topic_name, const char *app_name)
{
    if (!app_name || app_name[0] == '\0') {
        return -EINVAL;
    }
    return ipc_subscribe(topic_name, OWNER_SYSTEM, app_name);
}

int akira_ipc_unsubscribe(const char *topic_name, const char *app_name)
{
    if (!app_name) {
        return -EINVAL;
    }
    return ipc_unsubscribe(topic_name, OWNER_SYSTEM, app_name);
}

int akira_ipc_recv(const char *topic_name, const char *app_name,
                   void *buf, size_t len, k_timeout_t timeout)
{
    if (!app_name) {
        return -EINVAL;
    }
    return ipc_recv(topic_name, OWNER_SYSTEM, app_name, buf, len, timeout);
}

int akira_ipc_pending(const char *topic_name, const char *app_name)
{
    if (!app_name) {
        return -EINVAL;
    }
    return ipc_pending(topic_name, OWNER_SYSTEM, app_name);
}

int akira_ipc_publish(const char *topic_name, const void *data, size_t len)
{
    if (!topic_name || !data) {
        return -EINVAL;
    }
    if (len == 0 || len > CONFIG_AKIRA_IPC_MSG_MAX_SIZE) {
        return -EMSGSIZE;
    }
    if (!name_ok(topic_name, AKIRA_IPC_TOPIC_NAME_MAX)) {
        return -EINVAL;
    }

    /* Copy the payload (possibly app memory) before taking the lock. */
    akira_ipc_msg_t msg;
    msg.len = (uint16_t)len;
    memcpy(msg.data, data, len);

    if (ipc_lock() < 0) {
        return -EBUSY;
    }

    akira_ipc_topic_t *topic = find_topic(topic_name);
    if (!topic) {
        ipc_unlock();
        /* No subscribers yet — not an error, message is silently discarded */
        return 0;
    }

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

    ipc_unlock();
    return delivered;
}

void akira_ipc_cleanup_app(const char *app_name)
{
    if (!app_name || app_name[0] == '\0') {
        return;
    }
    if (ipc_lock() < 0) {
        LOG_ERR("IPC cleanup of '%s': lock unavailable, subscriptions kept", app_name);
        return;
    }

    for (int t = 0; t < CONFIG_AKIRA_IPC_MAX_TOPICS; t++) {
        akira_ipc_topic_t *topic = &g_ipc_topics[t];
        if (!topic->used) {
            continue;
        }

        /* System subscribers only: an app's subscriptions go by slot, and
         * only once its thread can no longer be waiting in one of them. */
        for (int s = 0; s < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; s++) {
            akira_ipc_sub_t *sub = &topic->subs[s];
            if (sub->used && sub->owner_slot == OWNER_SYSTEM &&
                strncmp(sub->app_name, app_name,
                        AKIRA_IPC_APP_NAME_MAX) == 0) {
                LOG_DBG("IPC cleanup: removing sub '%s' from topic '%s'",
                        app_name, topic->name);
                destroy_sub(sub);
            }
        }
        release_topic_if_empty(topic);
    }

    ipc_unlock();
}

/*===========================================================================*/
/* Public API — WASM apps (by runtime slot)                                  */
/*===========================================================================*/

int akira_ipc_subscribe_slot(int slot, const char *topic_name, const char *app_name)
{
    if (slot < 0 || slot > INT8_MAX) {
        return -EINVAL;
    }
    return ipc_subscribe(topic_name, slot, app_name);
}

int akira_ipc_unsubscribe_slot(int slot, const char *topic_name)
{
    if (slot < 0 || slot > INT8_MAX) {
        return -EINVAL;
    }
    return ipc_unsubscribe(topic_name, slot, NULL);
}

int akira_ipc_recv_slot(int slot, const char *topic_name,
                        void *buf, size_t len, k_timeout_t timeout)
{
    if (slot < 0 || slot > INT8_MAX) {
        return -EINVAL;
    }
    return ipc_recv(topic_name, slot, NULL, buf, len, timeout);
}

int akira_ipc_pending_slot(int slot, const char *topic_name)
{
    if (slot < 0 || slot > INT8_MAX) {
        return -EINVAL;
    }
    return ipc_pending(topic_name, slot, NULL);
}

void akira_ipc_cleanup_slot(int slot)
{
    if (slot < 0 || slot > INT8_MAX) {
        return;
    }
    if (ipc_lock() < 0) {
        LOG_ERR("IPC cleanup of slot %d: lock unavailable, subscriptions kept", slot);
        return;
    }

    for (int t = 0; t < CONFIG_AKIRA_IPC_MAX_TOPICS; t++) {
        akira_ipc_topic_t *topic = &g_ipc_topics[t];
        if (!topic->used) {
            continue;
        }
        for (int s = 0; s < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; s++) {
            akira_ipc_sub_t *sub = &topic->subs[s];
            if (sub->used && sub->owner_slot == slot) {
                destroy_sub(sub);
            }
        }
        release_topic_if_empty(topic);
    }

    ipc_unlock();
}

/*===========================================================================*/
/* App teardown                                                              */
/*===========================================================================*/

/* A stop was requested: wake the app if it is blocked in a receive. The
 * queue it waits on is empty, so the wake envelope goes straight to it; a
 * full queue means it is not waiting. Must not block: a busy lock is left to
 * the receive's own stop checks between slices. */
static void ipc_on_stop(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);
    if (k_mutex_lock(&g_ipc_mutex, K_MSEC(50)) != 0) {
        return;
    }
    for (int t = 0; t < CONFIG_AKIRA_IPC_MAX_TOPICS; t++) {
        akira_ipc_topic_t *topic = &g_ipc_topics[t];
        if (!topic->used) {
            continue;
        }
        for (int s = 0; s < CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC; s++) {
            akira_ipc_sub_t *sub = &topic->subs[s];
            if (sub->used && sub->owner_slot == slot) {
                (void)k_msgq_put(&sub->msgq, &g_ipc_wake, K_NO_WAIT);
            }
        }
    }
    ipc_unlock();
}

/* An app's subscriptions go with it, however it ends. */
static void ipc_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);
    akira_ipc_cleanup_slot(slot);
}

AKIRA_APP_RESOURCE_DEFINE(ipc, 50, ipc_on_stop, ipc_release);
