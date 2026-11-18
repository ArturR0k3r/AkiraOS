/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_ipc.h
 * @brief Inter-App Messaging — named topic pub/sub for WASM apps
 *
 * WASM apps subscribe to named topics, publish messages to them, and
 * receive messages sent by other apps.  Each subscription owns a private
 * k_msgq.  An app's backing buffer comes from PSRAM only on boards that have
 * it (never the internal-DRAM system heap); without PSRAM, the per-app cap
 * (AKIRA_IPC_MAX_SUBS_PER_APP) bounds what an app can take.
 *
 * Two kinds of subscriber: WASM apps, identified by runtime slot (the
 * *_slot functions; the native API uses these), and system code identified
 * by a name (the plain functions).  An app's subscriptions are released when
 * it ends, however it ends, and a stop wakes it out of a receive (-EINTR).
 *
 * Thread-safety: all APIs are safe to call from any Zephyr thread.  Topic
 * names must be shorter than AKIRA_IPC_TOPIC_NAME_MAX (else -EINVAL).  A
 * call that cannot get the IPC lock within 1 s returns -EBUSY.
 *
 * Limits (configurable via Kconfig):
 *   CONFIG_AKIRA_IPC_MAX_TOPICS          (default 8)
 *   CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC  (default 4)
 *   CONFIG_AKIRA_IPC_MSG_MAX_SIZE        (default 256 bytes)
 *   CONFIG_AKIRA_IPC_QUEUE_DEPTH         (default 4 messages)
 * @stability experimental
 * @since 1.4
 */

#ifndef AKIRA_IPC_H
#define AKIRA_IPC_H

#include <stdint.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pull compile-time limits from Kconfig with safe defaults */
#ifndef CONFIG_AKIRA_IPC_MAX_TOPICS
#define CONFIG_AKIRA_IPC_MAX_TOPICS 8
#endif

#ifndef CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC
#define CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC 4
#endif

#ifndef CONFIG_AKIRA_IPC_MSG_MAX_SIZE
#define CONFIG_AKIRA_IPC_MSG_MAX_SIZE 256
#endif

#ifndef CONFIG_AKIRA_IPC_QUEUE_DEPTH
#define CONFIG_AKIRA_IPC_QUEUE_DEPTH 4
#endif

#define AKIRA_IPC_TOPIC_NAME_MAX 32
#define AKIRA_IPC_APP_NAME_MAX   32

/** Subscriptions one WASM app may hold (each also pins a topic slot). */
#define AKIRA_IPC_MAX_SUBS_PER_APP \
    MIN(CONFIG_AKIRA_IPC_MAX_TOPICS, MAX(2, CONFIG_AKIRA_IPC_MAX_TOPICS / 4))

/** Single message envelope stored in the msgq */
typedef struct {
    uint16_t len;                            /**< Actual payload length */
    uint8_t  data[CONFIG_AKIRA_IPC_MSG_MAX_SIZE]; /**< Message payload */
} akira_ipc_msg_t;

/**
 * @brief Subscribe to a named topic
 *
 * Creates the topic if it does not yet exist.  A private k_msgq is
 * allocated for this (topic, app_name) pair.  Calling subscribe again
 * for the same pair is a no-op and returns 0.
 *
 * @param topic_name  Topic identifier (max AKIRA_IPC_TOPIC_NAME_MAX-1 chars)
 * @param app_name    Subscriber identity (usually the calling app's name)
 * @return 0 on success, -ENOMEM if tables are full, -EINVAL on bad args
 */
int akira_ipc_subscribe(const char *topic_name, const char *app_name);

/**
 * @brief Unsubscribe from a topic and release the msgq backing buffer
 *
 * @param topic_name  Topic name
 * @param app_name    Subscriber identity
 * @return 0 on success, -ENOENT if subscription not found
 */
int akira_ipc_unsubscribe(const char *topic_name, const char *app_name);

/**
 * @brief Publish a message to all subscribers of a topic
 *
 * Non-blocking: if a subscriber's queue is full the message is dropped
 * for that subscriber (logged as DBG).  Returns the number of subscribers
 * that received the message.
 *
 * @param topic_name  Topic name (must already have at least one subscriber)
 * @param data        Payload pointer
 * @param len         Payload length (must be <= CONFIG_AKIRA_IPC_MSG_MAX_SIZE)
 * @return Number of subscribers that received the message (>= 0),
 *         -EINVAL / -EMSGSIZE on bad args
 */
int akira_ipc_publish(const char *topic_name, const void *data, size_t len);

/**
 * @brief Receive the next message from a topic subscription
 *
 * @param topic_name  Topic name
 * @param app_name    Subscriber identity (must match subscribe call)
 * @param buf         Output buffer (at least len bytes)
 * @param len         Maximum bytes to read
 * @param timeout     K_NO_WAIT, K_FOREVER, or K_MSEC(n)
 * @return Bytes received (>= 0), -EAGAIN if timed out, negative on error
 */
int akira_ipc_recv(const char *topic_name, const char *app_name,
                   void *buf, size_t len, k_timeout_t timeout);

/**
 * @brief Query how many messages are pending in a subscription queue
 *
 * @param topic_name  Topic name
 * @param app_name    Subscriber identity
 * @return Pending message count (>= 0), negative on error
 */
int akira_ipc_pending(const char *topic_name, const char *app_name);

/**
 * @brief Remove all system subscriptions registered under a name
 *
 * Frees the msgq buffers of every system subscription whose subscriber
 * name is @p app_name.  Safe to call even if there are none.  WASM app
 * subscriptions are not touched: they are released by slot when the app
 * ends (akira_ipc_cleanup_slot()).
 *
 * @param app_name  Subscriber name whose subscriptions should be cleaned up
 */
void akira_ipc_cleanup_app(const char *app_name);

/**
 * @brief Subscribe a WASM app (runtime slot @p slot) to a topic
 *
 * As akira_ipc_subscribe(), but the subscription belongs to the app slot:
 * only that slot can receive from or remove it, and it is released when the
 * app ends.  @p app_name is kept for diagnostics.
 *
 * @return 0 on success, -ENOMEM if tables are full, the app already holds
 *         AKIRA_IPC_MAX_SUBS_PER_APP subscriptions or no PSRAM is left,
 *         -EINVAL on bad args, -EBUSY if the IPC lock is unavailable
 */
int akira_ipc_subscribe_slot(int slot, const char *topic_name, const char *app_name);

/** @brief Remove app slot @p slot's subscription to a topic (-ENOENT if none). */
int akira_ipc_unsubscribe_slot(int slot, const char *topic_name);

/**
 * @brief Receive the next message for app slot @p slot
 *
 * @return Bytes received (>= 0), -EAGAIN if timed out, -EINTR if the app is
 *         being stopped, -ENOENT if not subscribed, negative on error
 */
int akira_ipc_recv_slot(int slot, const char *topic_name,
                        void *buf, size_t len, k_timeout_t timeout);

/** @brief Pending message count for app slot @p slot (-ENOENT if not subscribed). */
int akira_ipc_pending_slot(int slot, const char *topic_name);

/** @brief Remove every subscription owned by app slot @p slot (idempotent). */
void akira_ipc_cleanup_slot(int slot);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_IPC_H */
