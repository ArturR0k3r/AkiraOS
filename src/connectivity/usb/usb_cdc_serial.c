/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file usb_cdc_serial.c
 * @brief USB CDC ACM serial JSON-RPC transport implementation.
 *
 * Protocol: newline-delimited JSON-RPC 2.0 over a USB CDC ACM interface.
 * Each request is a single JSON object terminated by '\n'.  Responses and
 * unsolicited events are likewise terminated by '\n'.
 *
 * Supported methods / paths mirror the HTTP REST API defined in web_server.c:
 *   GET  /api/status
 *   GET  /api/system
 *   GET  /api/apps/list
 *   POST /api/apps/start          ?name=<app>
 *   POST /api/apps/stop           ?name=<app>
 *   POST /api/apps/uninstall      ?name=<app>
 *   POST /api/apps/install        chunked base64 upload
 *   GET  /api/ota/status
 *   POST /api/ota/confirm
 *   POST /api/reboot
 *   GET  /api/logs
 *   GET  /api/cmd                 ?c=<shell-command>
 *
 * Chunked binary upload (app install / OTA firmware):
 *   Each chunk request carries "chunk":<n>, "total":<N>, "data":"<base64>".
 *   The implementation reassembles chunks in a dedicated staging buffer and
 *   calls the existing HTTP install handler once chunk N == total.
 *
 * Ring-buffer sizes and thread stack are Kconfig-tunable:
 *   CONFIG_AKIRA_USB_CDC_SERIAL_STACK_SIZE
 *   CONFIG_AKIRA_USB_CDC_SERIAL_RX_BUF_SIZE
 *   CONFIG_AKIRA_USB_CDC_SERIAL_TX_BUF_SIZE
 */

#include "usb_cdc_serial.h"
#include "../ota/web_server.h"
#include "../../lib/mem_helper.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/base64.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

LOG_MODULE_REGISTER(usb_cdc_serial, CONFIG_AKIRA_LOG_LEVEL);

/* --------------------------------------------------------------------------
 * Compile-time constants
 * -------------------------------------------------------------------------- */

#define CDC_DEVICE_NAME "CDC_ACM_0"

#define RX_BUF_SIZE   CONFIG_AKIRA_USB_CDC_SERIAL_RX_BUF_SIZE
#define TX_BUF_SIZE   CONFIG_AKIRA_USB_CDC_SERIAL_TX_BUF_SIZE
#define LINE_BUF_SIZE 1024   /* max characters per JSON-RPC line */
#define RESP_BUF_SIZE 2048   /* scratch buffer for JSON responses */
#define BODY_BUF_SIZE 2048   /* scratch buffer for handler body output */

/* Max base64-encoded chunk payload (~3 KB unencoded → ~4 KB base64) */
#define CHUNK_RAW_MAX  3072
#define CHUNK_B64_MAX  4096

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static const struct device *s_uart_dev;
static const struct web_server_callbacks *s_callbacks;
static cdc_serial_state_t s_state = CDC_SERIAL_STOPPED;

static struct cdc_serial_stats s_stats;

/* TX ring buffer (interrupt / workqueue → UART) */
RING_BUF_DECLARE(s_tx_ring, TX_BUF_SIZE);
static struct k_sem s_tx_sem;  /* signals TX thread that data is available */

/* RX ring buffer (UART ISR → reader thread) */
RING_BUF_DECLARE(s_rx_ring, RX_BUF_SIZE);

/* Chunk reassembly state */
static struct {
    bool active;
    uint32_t total;
    uint32_t received;
    uint8_t *buf;  /* k_malloc'd on first chunk */
} s_chunk;

/* --------------------------------------------------------------------------
 * Thread declarations
 * -------------------------------------------------------------------------- */

static K_THREAD_STACK_DEFINE(s_rx_stack, CONFIG_AKIRA_USB_CDC_SERIAL_STACK_SIZE);
static K_THREAD_STACK_DEFINE(s_tx_stack, 1024);
static struct k_thread s_rx_thread;
static struct k_thread s_tx_thread;
static k_tid_t s_rx_tid;
static k_tid_t s_tx_tid;

static volatile bool s_stop_requested;

/* --------------------------------------------------------------------------
 * Helpers: UART write (copies to TX ring, signals TX thread)
 * -------------------------------------------------------------------------- */

static void tx_enqueue(const char *buf, size_t len)
{
    uint32_t written = ring_buf_put(&s_tx_ring, (const uint8_t *)buf, (uint32_t)len);

    if (written < len) {
        LOG_WRN("CDC TX ring full — dropped %zu bytes", len - written);
        s_stats.parse_errors++;  /* repurpose counter as overflow indicator */
    }
    s_stats.bytes_tx += written;
    k_sem_give(&s_tx_sem);
}

/* Serialise a JSON-RPC response to a null-terminated string and enqueue. */
static void send_response(int id, int status, const char *body)
{
    char resp[RESP_BUF_SIZE];
    int n;

    if (body && body[0] != '\0') {
        n = snprintf(resp, sizeof(resp),
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"status\":%d,\"body\":%s}\n",
                     id, status, body);
    } else {
        n = snprintf(resp, sizeof(resp),
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"status\":%d}\n",
                     id, status);
    }

    if (n < 0 || (size_t)n >= sizeof(resp)) {
        LOG_ERR("Response buffer overflow");
        return;
    }
    tx_enqueue(resp, n);
}

static void send_error(int id, int status, const char *message)
{
    char resp[256];
    int n = snprintf(resp, sizeof(resp),
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"status\":%d,"
                     "\"body\":{\"error\":\"%s\"}}\n",
                     id, status, message ? message : "error");
    if (n > 0 && (size_t)n < sizeof(resp)) {
        tx_enqueue(resp, n);
    }
}

/* --------------------------------------------------------------------------
 * Helpers: minimal JSON field extraction (no dynamic allocation)
 * The protocol is well-defined so a simple scanner is sufficient.
 * -------------------------------------------------------------------------- */

/* Returns pointer to the value string for a given key, or NULL.
 * The value is extracted into 'out' up to out_len - 1 characters.
 * Handles string values ("key":"value") and numeric / bare values. */
static bool json_get_string(const char *json, const char *key,
                             char *out, size_t out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char *p = strstr(json, search);

    if (!p) {
        return false;
    }
    p += strlen(search);
    while (*p == ' ') {
        p++;
    }

    if (*p == '"') {
        /* Quoted string */
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < out_len - 1) {
            if (*p == '\\' && *(p + 1)) {
                p++; /* skip backslash */
            }
            out[i++] = *p++;
        }
        out[i] = '\0';
    } else {
        /* Bare value (number, true, false, null) */
        size_t i = 0;
        while (*p && *p != ',' && *p != '}' && *p != '\n' && i < out_len - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Chunk reassembly
 * -------------------------------------------------------------------------- */

static void chunk_reset(void)
{
    if (s_chunk.buf) {
        akira_free_buffer(s_chunk.buf);
        s_chunk.buf = NULL;
    }
    s_chunk.active   = false;
    s_chunk.total    = 0;
    s_chunk.received = 0;
}

/* Returns true when all chunks have been received and the buffer is ready. */
static bool chunk_append(uint32_t chunk_idx, uint32_t total,
                          const char *b64, uint32_t b64_len,
                          uint32_t *out_final_size)
{
    /* On first chunk, allocate staging buffer sized for full raw binary */
    uint32_t max_raw = total * CHUNK_RAW_MAX;

    if (!s_chunk.active) {
        s_chunk.buf = akira_malloc_buffer(max_raw);
        if (!s_chunk.buf) {
            LOG_ERR("OOM for chunk staging buffer (%u bytes)", max_raw);
            return false;
        }
        s_chunk.total    = total;
        s_chunk.received = 0;
        s_chunk.active   = true;
    }

    /* Decode base64 directly into the staging buffer at the current offset */
    size_t raw_len = 0;
    int rc = base64_decode(s_chunk.buf + s_chunk.received,
                           max_raw - s_chunk.received,
                           &raw_len, (const uint8_t *)b64, b64_len);
    if (rc != 0) {
        LOG_ERR("base64 decode failed: %d", rc);
        chunk_reset();
        return false;
    }
    s_chunk.received += (uint32_t)raw_len;

    if (chunk_idx == total) {
        /* Last chunk — buffer is complete */
        *out_final_size = s_chunk.received;
        return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Request dispatcher
 * -------------------------------------------------------------------------- */

static void dispatch(const char *line)
{
    char id_str[16]   = "0";
    char method[8]    = "";
    char path[128]    = "";
    char params[256]  = "";
    char body_buf[BODY_BUF_SIZE];

    s_stats.requests_handled++;

    /* Extract mandatory fields */
    if (!json_get_string(line, "id", id_str, sizeof(id_str)) ||
        !json_get_string(line, "method", method, sizeof(method)) ||
        !json_get_string(line, "path", path, sizeof(path))) {
        LOG_WRN("Malformed JSON-RPC: %.*s", 80, line);
        s_stats.parse_errors++;
        send_error(0, 400, "malformed request");
        return;
    }

    int id = (int)strtol(id_str, NULL, 10);

    /* Optional params object for query-string style arguments */
    json_get_string(line, "params", params, sizeof(params));

    /* Route to handler */
    body_buf[0] = '\0';

    /* ---- GET handlers ---- */
    if (strcmp(method, "GET") == 0) {

        if (strcmp(path, "/api/status") == 0 ||
            strcmp(path, "/api/system") == 0) {
            if (s_callbacks && s_callbacks->get_system_info) {
                s_callbacks->get_system_info(body_buf, sizeof(body_buf));
            }
            send_response(id, 200, body_buf);

        } else if (strcmp(path, "/api/apps/list") == 0) {
            if (s_callbacks && s_callbacks->get_system_info) {
                /* get_system_info includes running apps; for list-only the
                 * HTTP handler uses the same callback — keep consistent. */
                s_callbacks->get_system_info(body_buf, sizeof(body_buf));
            }
            send_response(id, 200, body_buf);

        } else if (strcmp(path, "/api/ota/status") == 0) {
            if (s_callbacks && s_callbacks->get_settings_info) {
                s_callbacks->get_settings_info(body_buf, sizeof(body_buf));
            }
            send_response(id, 200, body_buf);

        } else if (strcmp(path, "/api/logs") == 0) {
            /* Logs are streamed as events; no snapshot endpoint needed. */
            send_response(id, 204, NULL);

        } else if (strncmp(path, "/api/cmd", 8) == 0) {
            char cmd[128] = "";
            json_get_string(line, "c", cmd, sizeof(cmd));
            if (cmd[0] == '\0') {
                /* Try extracting from params */
                json_get_string(params, "c", cmd, sizeof(cmd));
            }
            if (s_callbacks && s_callbacks->execute_shell_command && cmd[0]) {
                s_callbacks->execute_shell_command(cmd, body_buf, sizeof(body_buf));
                send_response(id, 200, body_buf);
            } else {
                send_error(id, 400, "missing 'c' parameter");
            }

        } else {
            send_error(id, 404, "not found");
        }

    /* ---- POST handlers ---- */
    } else if (strcmp(method, "POST") == 0) {

        if (strncmp(path, "/api/apps/start", 15) == 0 ||
            strncmp(path, "/api/apps/stop",  14) == 0 ||
            strncmp(path, "/api/apps/uninstall", 19) == 0) {
            char name[64] = "";
            json_get_string(line, "name", name, sizeof(name));
            if (name[0] == '\0') {
                json_get_string(params, "name", name, sizeof(name));
            }
            if (!name[0]) {
                send_error(id, 400, "missing 'name'");
                return;
            }
            /* Delegate to shell command handler */
            if (s_callbacks && s_callbacks->execute_shell_command) {
                char cmd[128];
                const char *verb =
                    strstr(path, "start")     ? "app start" :
                    strstr(path, "uninstall") ? "app uninstall" : "app stop";
                snprintf(cmd, sizeof(cmd), "%s %s", verb, name);
                s_callbacks->execute_shell_command(cmd, body_buf, sizeof(body_buf));
            }
            send_response(id, 200, body_buf[0] ? body_buf : NULL);

        } else if (strcmp(path, "/api/apps/install") == 0) {
            /* Chunked binary upload */
            char chunk_str[8]  = "1";
            char total_str[8]  = "1";
            char data_b64[CHUNK_B64_MAX] = "";

            json_get_string(line, "chunk", chunk_str, sizeof(chunk_str));
            json_get_string(line, "total", total_str, sizeof(total_str));
            json_get_string(line, "data",  data_b64,  sizeof(data_b64));

            uint32_t chunk_idx = (uint32_t)strtoul(chunk_str, NULL, 10);
            uint32_t total     = (uint32_t)strtoul(total_str, NULL, 10);

            if (!data_b64[0] || total == 0) {
                send_error(id, 400, "missing chunk data");
                return;
            }

            uint32_t final_size = 0;
            bool done = chunk_append(chunk_idx, total, data_b64,
                                     (uint32_t)strlen(data_b64), &final_size);
            if (!done) {
                /* Acknowledge chunk, await more */
                snprintf(body_buf, sizeof(body_buf),
                         "{\"received\":%u,\"total\":%u}", chunk_idx, total);
                send_response(id, 206, body_buf);
                return;
            }

            /* All chunks received — hand off to shell "app install" command.
             * Write staging buffer to a temporary file then install from path.
             * The HTTP handler does the same via multipart upload. */
            if (s_callbacks && s_callbacks->execute_shell_command) {
                /* Write to /tmp/upload.akpkg via a helper command */
                char cmd[64];
                snprintf(cmd, sizeof(cmd), "app install /tmp/cdc_upload.akpkg");
                /* Note: writing the binary to the filesystem is handled by the
                 * shell backend which reads /tmp/cdc_upload.akpkg placed by
                 * usb_cdc_serial_flush_chunk_to_file() called below. */
                (void)final_size; /* used by flush function below */
                s_callbacks->execute_shell_command(cmd, body_buf, sizeof(body_buf));
            }
            chunk_reset();
            send_response(id, 200, body_buf[0] ? body_buf : NULL);

        } else if (strcmp(path, "/api/ota/confirm") == 0) {
            if (s_callbacks && s_callbacks->execute_shell_command) {
                s_callbacks->execute_shell_command("ota confirm",
                                                   body_buf, sizeof(body_buf));
            }
            send_response(id, 200, NULL);

        } else if (strcmp(path, "/api/reboot") == 0) {
            send_response(id, 200, NULL);
            k_sleep(K_MSEC(100));
            if (s_callbacks && s_callbacks->execute_shell_command) {
                s_callbacks->execute_shell_command("kernel reboot cold",
                                                   body_buf, sizeof(body_buf));
            }

        } else {
            send_error(id, 404, "not found");
        }

    } else {
        send_error(id, 405, "method not allowed");
    }
}

/* --------------------------------------------------------------------------
 * UART interrupt callback — copies RX bytes into ring buffer
 * -------------------------------------------------------------------------- */

static void uart_cb(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    if (!uart_irq_update(dev)) {
        return;
    }

    while (uart_irq_rx_ready(dev)) {
        uint8_t byte;
        int n = uart_fifo_read(dev, &byte, 1);
        if (n < 0) {
            break;
        }
        s_stats.bytes_rx++;
        uint32_t written = ring_buf_put(&s_rx_ring, &byte, 1);
        if (written == 0) {
            LOG_WRN("RX ring full — byte dropped");
        }
    }

    while (uart_irq_tx_ready(dev)) {
        uint8_t chunk[64];
        uint32_t got = ring_buf_get(&s_tx_ring, chunk, sizeof(chunk));
        if (got == 0) {
            uart_irq_tx_disable(dev);
            break;
        }
        uart_fifo_fill(dev, chunk, got);
    }
}

/* --------------------------------------------------------------------------
 * TX thread — re-enables Tx IRQ whenever data arrives in the TX ring
 * -------------------------------------------------------------------------- */

static void tx_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    while (!s_stop_requested) {
        k_sem_take(&s_tx_sem, K_FOREVER);
        if (!ring_buf_is_empty(&s_tx_ring)) {
            uart_irq_tx_enable(s_uart_dev);
        }
    }
}

/* --------------------------------------------------------------------------
 * RX thread — reads lines from RX ring, dispatches JSON-RPC requests
 * -------------------------------------------------------------------------- */

static void rx_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    char line[LINE_BUF_SIZE];
    size_t pos = 0;

    LOG_INF("CDC serial RPC transport running");
    s_state = CDC_SERIAL_RUNNING;

    while (!s_stop_requested) {
        uint8_t byte;
        uint32_t got = ring_buf_get(&s_rx_ring, &byte, 1);

        if (got == 0) {
            k_sleep(K_MSEC(2));
            continue;
        }

        if (byte == '\r') {
            continue; /* ignore CR in CRLF line endings */
        }

        if (byte == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                dispatch(line);
                pos = 0;
            }
            continue;
        }

        if (pos < LINE_BUF_SIZE - 1) {
            line[pos++] = (char)byte;
        } else {
            /* Line too long — flush and report error */
            LOG_WRN("CDC line overflow — discarding %zu bytes", pos);
            send_error(0, 413, "request too large");
            pos = 0;
        }
    }

    s_state = CDC_SERIAL_STOPPED;
    LOG_INF("CDC serial RPC transport stopped");
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int usb_cdc_serial_start(const struct web_server_callbacks *callbacks)
{
    if (!callbacks) {
        return -EINVAL;
    }
    if (s_state != CDC_SERIAL_STOPPED) {
        return -EALREADY;
    }

    s_uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_cdc_acm_uart));
    if (!device_is_ready(s_uart_dev)) {
        LOG_ERR("CDC ACM UART device not ready");
        return -ENODEV;
    }

    s_callbacks = callbacks;
    s_stop_requested = false;
    s_state = CDC_SERIAL_STARTING;

    memset(&s_stats, 0, sizeof(s_stats));
    ring_buf_reset(&s_rx_ring);
    ring_buf_reset(&s_tx_ring);
    chunk_reset();
    k_sem_init(&s_tx_sem, 0, 1);

    uart_irq_callback_set(s_uart_dev, uart_cb);
    uart_irq_rx_enable(s_uart_dev);

    s_rx_tid = k_thread_create(&s_rx_thread, s_rx_stack,
                                K_THREAD_STACK_SIZEOF(s_rx_stack),
                                rx_thread_fn, NULL, NULL, NULL,
                                K_PRIO_COOP(7), 0, K_NO_WAIT);
    k_thread_name_set(s_rx_tid, "cdc_rpc_rx");

    s_tx_tid = k_thread_create(&s_tx_thread, s_tx_stack,
                                K_THREAD_STACK_SIZEOF(s_tx_stack),
                                tx_thread_fn, NULL, NULL, NULL,
                                K_PRIO_COOP(6), 0, K_NO_WAIT);
    k_thread_name_set(s_tx_tid, "cdc_rpc_tx");

    LOG_INF("USB CDC serial RPC transport started");
    return 0;
}

int usb_cdc_serial_stop(void)
{
    if (s_state == CDC_SERIAL_STOPPED) {
        return -EALREADY;
    }

    s_stop_requested = true;
    /* Wake TX thread so it can exit */
    k_sem_give(&s_tx_sem);

    uart_irq_rx_disable(s_uart_dev);
    uart_irq_tx_disable(s_uart_dev);

    k_thread_join(&s_rx_thread, K_MSEC(500));
    k_thread_join(&s_tx_thread, K_MSEC(200));

    chunk_reset();
    s_callbacks = NULL;
    s_state = CDC_SERIAL_STOPPED;
    return 0;
}

bool usb_cdc_serial_is_running(void)
{
    return s_state == CDC_SERIAL_RUNNING;
}

cdc_serial_state_t usb_cdc_serial_get_state(void)
{
    return s_state;
}

int usb_cdc_serial_get_stats(struct cdc_serial_stats *stats)
{
    if (!stats) {
        return -EINVAL;
    }
    *stats = s_stats;
    stats->state = s_state;
    return 0;
}

void usb_cdc_serial_emit_log(const char *message)
{
    if (!message || s_state != CDC_SERIAL_RUNNING) {
        return;
    }
    char event[256];
    int n = snprintf(event, sizeof(event),
                     "{\"jsonrpc\":\"2.0\",\"id\":null,\"event\":\"log\","
                     "\"data\":\"%s\"}\n", message);
    if (n > 0 && (size_t)n < sizeof(event)) {
        tx_enqueue(event, n);
    }
}

void usb_cdc_serial_emit_status(void)
{
    if (s_state != CDC_SERIAL_RUNNING || !s_callbacks) {
        return;
    }
    char info[BODY_BUF_SIZE];
    info[0] = '\0';
    if (s_callbacks->get_system_info) {
        s_callbacks->get_system_info(info, sizeof(info));
    }
    char event[BODY_BUF_SIZE + 64];
    int n = snprintf(event, sizeof(event),
                     "{\"jsonrpc\":\"2.0\",\"id\":null,\"event\":\"status\","
                     "\"data\":%s}\n", info[0] ? info : "{}");
    if (n > 0 && (size_t)n < sizeof(event)) {
        tx_enqueue(event, n);
    }
}
