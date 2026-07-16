/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_USB_CDC_SERIAL_H
#define AKIRA_USB_CDC_SERIAL_H

/**
 * @file usb_cdc_serial.h
 * @brief USB CDC ACM serial JSON-RPC transport for AkiraOS.
 *
 * Exposes the same management API as the HTTP web server over a USB CDC ACM
 * (virtual COM port) using newline-delimited JSON-RPC 2.0.  This allows a
 * browser running the hosted web UI (e.g. lab.akiraos.io) to connect via the
 * Web Serial API (Chrome / Edge 89+) without requiring WiFi to be active.
 *
 * Protocol summary (newline-delimited JSON-RPC 2.0):
 *
 *   Request (host → device):
 *     {"jsonrpc":"2.0","id":<int>,"method":"<HTTP-method>","path":"<path>"}
 *     {"jsonrpc":"2.0","id":<int>,"method":"POST","path":"/api/apps/install",
 *      "chunk":<n>,"total":<N>,"data":"<base64>"}
 *
 *   Response (device → host):
 *     {"jsonrpc":"2.0","id":<int>,"status":<http-code>,"body":<json>}
 *
 *   Unsolicited events (device → host):
 *     {"jsonrpc":"2.0","id":null,"event":"log","data":"<message>"}
 *     {"jsonrpc":"2.0","id":null,"event":"status","data":{...}}
 *
 * Method names mirror the HTTP verb used on the existing REST API so the
 * same web front-end code can target either transport.
 *
 * Enabled by CONFIG_AKIRA_USB_CDC_SERIAL.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — matches web_server.h */
struct web_server_callbacks;

/**
 * @brief CDC serial transport states.
 */
typedef enum {
    CDC_SERIAL_STOPPED = 0,
    CDC_SERIAL_STARTING,
    CDC_SERIAL_RUNNING,  /**< USB enumerated + host port open */
    CDC_SERIAL_ERROR,
} cdc_serial_state_t;

/**
 * @brief CDC serial runtime statistics.
 */
struct cdc_serial_stats {
    uint32_t requests_handled;   /**< JSON-RPC requests processed */
    uint32_t parse_errors;       /**< Malformed JSON or unknown method */
    uint32_t bytes_rx;           /**< Total bytes received from host */
    uint32_t bytes_tx;           /**< Total bytes sent to host */
    cdc_serial_state_t state;
};

/**
 * @brief Initialise and start the CDC serial JSON-RPC transport.
 *
 * Registers a USB CDC ACM interface, spawns a reader thread, and begins
 * servicing JSON-RPC requests using the provided callbacks (the same
 * callback struct used by the HTTP web server).
 *
 * @param callbacks  Pointer to handler callbacks.  Must remain valid for the
 *                   lifetime of the transport.  Must not be NULL.
 * @return 0 on success, -EINVAL if callbacks is NULL,
 *         -EALREADY if already running, negative errno otherwise.
 */
int usb_cdc_serial_start(const struct web_server_callbacks *callbacks);

/**
 * @brief Stop the CDC serial transport.
 *
 * Signals the reader thread to exit and waits for it to finish.
 *
 * @return 0 on success, -EALREADY if already stopped.
 */
int usb_cdc_serial_stop(void);

/**
 * @brief Query whether the transport is active and the host has opened the port.
 *
 * @return true if state is CDC_SERIAL_RUNNING.
 */
bool usb_cdc_serial_is_running(void);

/**
 * @brief Get current state.
 */
cdc_serial_state_t usb_cdc_serial_get_state(void);

/**
 * @brief Get runtime statistics.
 *
 * @param stats  Output buffer; must not be NULL.
 * @return 0 on success, -EINVAL if stats is NULL.
 */
int usb_cdc_serial_get_stats(struct cdc_serial_stats *stats);

/**
 * @brief Emit an unsolicited log event to the host.
 *
 * Sends a JSON-RPC notification:
 *   {"jsonrpc":"2.0","id":null,"event":"log","data":"<message>"}
 *
 * The call is non-blocking; if the TX ring is full the message is silently
 * dropped (same behaviour as web_server_broadcast_log).
 *
 * @param message  Null-terminated log string.
 */
void usb_cdc_serial_emit_log(const char *message);

/**
 * @brief Emit an unsolicited status event to the host.
 *
 * The status JSON payload is obtained by calling the get_system_info
 * callback passed to usb_cdc_serial_start().
 *
 * Sends a JSON-RPC notification:
 *   {"jsonrpc":"2.0","id":null,"event":"status","data":{...}}
 */
void usb_cdc_serial_emit_status(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_USB_CDC_SERIAL_H */
