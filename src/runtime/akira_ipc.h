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
 * k_msgq.  The backing buffer is PSRAM-allocated to avoid exhausting SRAM
 * on low-memory targets.
 *
 * Thread-safety: all APIs are safe to call from any Zephyr thread.
 * The cleanup call (akira_ipc_cleanup_app) must be hooked into the app
 * lifecycle so stale queues are released when an app exits.
 *
 * Limits (configurable via Kconfig):
 *   CONFIG_AKIRA_IPC_MAX_TOPICS          (default 8)
 *   CONFIG_AKIRA_IPC_MAX_SUBS_PER_TOPIC  (default 4)
 *   CONFIG_AKIRA_IPC_MSG_MAX_SIZE        (default 256 bytes)
 *   CONFIG_AKIRA_IPC_QUEUE_DEPTH         (default 4 messages)
 */

#ifndef AKIRA_IPC_H
#define AKIRA_IPC_H

#include <stdint.h>
#include <stddef.h>
#include <zephyr/kernel.h>

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
 * @brief Remove all subscriptions owned by an app (call on app exit/crash)
 *
 * Frees all PSRAM msgq buffers associated with the app.  Safe to call
 * even if the app has no subscriptions.
 *
 * @param app_name  App whose subscriptions should be cleaned up
 */
void akira_ipc_cleanup_app(const char *app_name);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_IPC_H */
