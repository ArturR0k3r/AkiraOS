/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "catalog_net.h"
#include <connectivity/net/net_stream.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t ring_capacity(const uint8_t *buf)
{
    uint32_t capacity;
    memcpy(&capacity, buf + 8, 4);
    return capacity;
}

static uint32_t ring_write_idx(const uint8_t *buf)
{
    uint32_t idx;
    memcpy(&idx, buf + 0, 4);
    return idx;
}

static uint32_t ring_read_idx(const uint8_t *buf)
{
    uint32_t idx;
    memcpy(&idx, buf + 4, 4);
    return idx;
}

void catalog_ring_init(uint8_t *buf, uint32_t total_size)
{
    uint32_t zero = 0;
    uint32_t capacity = total_size - CATALOG_NET_RING_HDR_SIZE;

    memcpy(buf + 0, &zero, 4);      /* write_idx */
    memcpy(buf + 4, &zero, 4);      /* read_idx */
    memcpy(buf + 8, &capacity, 4);  /* capacity */
    memcpy(buf + 12, &zero, 4);     /* flags */
}

int catalog_ring_write(uint8_t *buf, const uint8_t *msg, uint16_t msg_len)
{
    uint32_t capacity = ring_capacity(buf);
    uint32_t w = ring_write_idx(buf);
    uint32_t r = ring_read_idx(buf);
    uint32_t used = w - r;
    uint32_t free_space = capacity - used;
    uint32_t framed_len = 2u + msg_len;

    if (framed_len > free_space) {
        return -ENOSPC;
    }

    uint8_t *data = buf + CATALOG_NET_RING_HDR_SIZE;
    uint8_t len_bytes[2] = { (uint8_t)(msg_len & 0xFF), (uint8_t)(msg_len >> 8) };

    for (uint32_t i = 0; i < 2; i++) {
        data[(w + i) % capacity] = len_bytes[i];
    }
    for (uint32_t i = 0; i < msg_len; i++) {
        data[(w + 2 + i) % capacity] = msg[i];
    }

    uint32_t new_w = w + framed_len;
    memcpy(buf + 0, &new_w, 4);
    return 0;
}

int catalog_ring_read(uint8_t *buf, uint8_t *out, uint32_t out_size)
{
    uint32_t capacity = ring_capacity(buf);
    uint32_t w = ring_write_idx(buf);
    uint32_t r = ring_read_idx(buf);

    if (w == r) {
        return -ENODATA;
    }

    uint8_t *data = buf + CATALOG_NET_RING_HDR_SIZE;
    uint16_t msg_len = (uint16_t)data[r % capacity] |
                        ((uint16_t)data[(r + 1) % capacity] << 8);

    if (msg_len > out_size) {
        return -ENOBUFS;
    }

    for (uint32_t i = 0; i < msg_len; i++) {
        out[i] = data[(r + 2 + i) % capacity];
    }

    uint32_t new_r = r + 2u + msg_len;
    memcpy(buf + 4, &new_r, 4);
    return (int)msg_len;
}

#define CATALOG_TX_BUF_SIZE 512
/* GitHub's redirect responses run ~5KB, almost all headers (large CSP
 * header) — measured via openssl s_client against the real endpoint. */
#define CATALOG_RX_BUF_SIZE 8192
#define CATALOG_HDR_BUF_SIZE 6144
#define CATALOG_LOCATION_SIZE 256
#define CATALOG_MAX_REDIRECTS 3

/* Shared by every path below — only one HTTPS call (one-shot or session) is
 * ever in flight at a time in this codebase, so these don't need to be
 * per-call locals. */
static uint8_t g_tx_buf[CATALOG_TX_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
static uint8_t g_rx_buf[CATALOG_RX_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
static uint8_t g_hdr_buf[CATALOG_HDR_BUF_SIZE] __attribute__((section(".ext_ram.bss")));

/* Opens a TLS stream, binds the shared TX/RX rings, and blocks until
 * NET_EVT_CONNECTED or @p timeout_ms elapses. Used by both the one-shot
 * catalog_https_get() path and the keep-alive session API below. */
static int open_and_connect(const char *host, uint16_t port, int timeout_ms)
{
    int init_ret = net_stream_init();
    if (init_ret < 0) {
        return init_ret;
    }

    int handle = net_stream_open(NET_TYPE_TLS);
    if (handle < 0) {
        return handle;
    }

    catalog_ring_init(g_tx_buf, sizeof(g_tx_buf));
    catalog_ring_init(g_rx_buf, sizeof(g_rx_buf));

    int ret = net_stream_tx_bind(handle, g_tx_buf, sizeof(g_tx_buf));
    if (ret < 0) {
        net_stream_close(handle);
        return ret;
    }
    ret = net_stream_rx_bind(handle, g_rx_buf, sizeof(g_rx_buf));
    if (ret < 0) {
        net_stream_close(handle);
        return ret;
    }

    ret = net_stream_connect(handle, host, port);
    if (ret < 0) {
        net_stream_close(handle);
        return ret;
    }

    int64_t deadline = k_uptime_get() + timeout_ms;
    bool connected = false;
    while (k_uptime_get() < deadline) {
        struct net_event evt;
        int evt_type = net_stream_event_pop(&evt);
        if (evt_type == NET_EVT_CONNECTED) {
            connected = true;
            break;
        }
        if (evt_type == NET_EVT_ERROR) {
            net_stream_close(handle);
            return -(int)evt.extra;
        }
        k_sleep(K_MSEC(20));
    }
    if (!connected) {
        net_stream_close(handle);
        return -ETIMEDOUT;
    }
    return handle;
}

/* Performs one GET, following no redirects itself — just reports the status
 * line and Location header (if any) back to the caller. */
static int catalog_https_get_once(const char *host, uint16_t port, const char *path,
                                   uint8_t *out, uint32_t out_size, int timeout_ms,
                                   int *http_status, char *location, size_t location_size)
{
    uint8_t *tx_buf = g_tx_buf;
    uint8_t *rx_buf = g_rx_buf;
    uint8_t *hdr_buf = g_hdr_buf;

    *http_status = 0;
    location[0] = '\0';

    int handle = open_and_connect(host, port, timeout_ms);
    if (handle < 0) {
        return handle;
    }

    int64_t deadline;
    int ret;

    /* Send GET request */
    char req[CATALOG_TX_BUF_SIZE - CATALOG_NET_RING_HDR_SIZE - 2];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    ret = catalog_ring_write(tx_buf, (const uint8_t *)req, (uint16_t)req_len);
    if (ret < 0) {
        net_stream_close(handle);
        return ret;
    }
    ret = net_stream_tx_flush(handle);
    if (ret < 0) {
        net_stream_close(handle);
        return ret;
    }

    /* Collect response until NET_EVT_DISCONNECTED or timeout. Headers are
     * accumulated separately (they can span multiple ring messages) until
     * "\r\n\r\n" is found; only then is the status line / Location header
     * parsed and any trailing body bytes copied to @p out. */
    uint32_t hdr_len = 0;
    uint32_t body_len = 0;
    bool headers_done = false;
    bool disconnected = false;
    deadline = k_uptime_get() + timeout_ms;

    while (k_uptime_get() < deadline && !disconnected) {
        struct net_event evt;
        int evt_type = net_stream_event_pop(&evt);
        if (evt_type == NET_EVT_DISCONNECTED) {
            disconnected = true;
        } else if (evt_type == NET_EVT_ERROR) {
            net_stream_close(handle);
            return -(int)evt.extra;
        }

        uint8_t chunk[1500]; /* matches CONFIG_AKIRA_NET_MAX_MSG_SIZE default */
        int n;
        while ((n = catalog_ring_read(rx_buf, chunk, sizeof(chunk))) > 0) {
            if (!headers_done) {
                uint32_t copy = (uint32_t)n;
                if (copy > CATALOG_HDR_BUF_SIZE - 1 - hdr_len) {
                    copy = CATALOG_HDR_BUF_SIZE - 1 - hdr_len;
                }
                memcpy(hdr_buf + hdr_len, chunk, copy);
                hdr_len += copy;
                hdr_buf[hdr_len] = '\0';

                char *sep = strstr((char *)hdr_buf, "\r\n\r\n");
                if (sep) {
                    headers_done = true;
                    sscanf((char *)hdr_buf, "HTTP/%*d.%*d %d", http_status);

                    char *loc = strstr((char *)hdr_buf, "Location: ");
                    if (loc) {
                        loc += strlen("Location: ");
                        char *eol = strstr(loc, "\r\n");
                        size_t loc_len = eol ? (size_t)(eol - loc) : strlen(loc);
                        if (loc_len >= location_size) {
                            loc_len = location_size - 1;
                        }
                        memcpy(location, loc, loc_len);
                        location[loc_len] = '\0';
                    }

                    uint8_t *body_start = (uint8_t *)sep + 4;
                    uint32_t body_in_hdr = (uint32_t)((hdr_buf + hdr_len) - body_start);
                    if (body_in_hdr > out_size - body_len) {
                        body_in_hdr = out_size - body_len;
                    }
                    memcpy(out + body_len, body_start, body_in_hdr);
                    body_len += body_in_hdr;
                }
            } else {
                uint32_t copy = (uint32_t)n;
                if (copy > out_size - body_len) {
                    copy = out_size - body_len;
                }
                memcpy(out + body_len, chunk, copy);
                body_len += copy;
            }
        }
        if (!disconnected) {
            k_sleep(K_MSEC(20));
        }
    }

    net_stream_close(handle);
    if (!headers_done && body_len == 0) {
        return -ETIMEDOUT;
    }
    return (int)body_len;
}

int catalog_https_get(const char *host, uint16_t port, const char *path,
                      uint8_t *out, uint32_t out_size, int timeout_ms)
{
    char cur_host[128];
    char cur_path[256];
    static char location[CATALOG_LOCATION_SIZE];

    strncpy(cur_host, host, sizeof(cur_host) - 1);
    cur_host[sizeof(cur_host) - 1] = '\0';
    strncpy(cur_path, path, sizeof(cur_path) - 1);
    cur_path[sizeof(cur_path) - 1] = '\0';

    for (int hop = 0; hop <= CATALOG_MAX_REDIRECTS; hop++) {
        int status = 0;
        int ret = catalog_https_get_once(cur_host, port, cur_path, out, out_size,
                                          timeout_ms, &status, location, sizeof(location));
        if (ret < 0) {
            return ret;
        }
        if (status < 300 || status >= 400 || location[0] == '\0') {
            return ret;
        }
        if (hop == CATALOG_MAX_REDIRECTS) {
            return -ELOOP;
        }

        const char *p = location;
        if (strncmp(p, "https://", 8) == 0) {
            p += 8;
            const char *slash = strchr(p, '/');
            if (!slash) {
                return -EPROTO;
            }
            size_t hlen = (size_t)(slash - p);
            if (hlen >= sizeof(cur_host)) {
                hlen = sizeof(cur_host) - 1;
            }
            memcpy(cur_host, p, hlen);
            cur_host[hlen] = '\0';
            strncpy(cur_path, slash, sizeof(cur_path) - 1);
            cur_path[sizeof(cur_path) - 1] = '\0';
        } else if (p[0] == '/') {
            strncpy(cur_path, p, sizeof(cur_path) - 1);
            cur_path[sizeof(cur_path) - 1] = '\0';
        } else {
            return -EPROTO;
        }
    }
    return -ELOOP;
}

/* =========================================================================
 * Keep-alive session API
 *
 * TLS handshakes against console.app.akiraos.dev routinely take close to
 * CONFIG_NET_SOCKETS_CONNECT_TIMEOUT (15s) to complete on this hardware — a
 * slow TLS negotiation, not a scheduler stall (the CPU sits idle waiting on
 * the network for that whole window). catalog_https_get() above pays that
 * cost on every call since it always sends "Connection: close" and tears
 * the stream down. Callers that need more than one request in a row
 * (catalogue fetch + app download) should use this session API instead to
 * pay it once.
 * ========================================================================= */

/* Case-insensitive search for a header line's value; @p key must include the
 * trailing ':'. Returns a pointer to the first non-space char of the value,
 * or NULL if not found. */
static const char *find_header_value(const char *headers, const char *key)
{
    size_t key_len = strlen(key);
    for (const char *p = headers; *p; p++) {
        if (strncasecmp(p, key, key_len) == 0) {
            p += key_len;
            while (*p == ' ') {
                p++;
            }
            return p;
        }
    }
    return NULL;
}

int catalog_https_open(const char *host, uint16_t port, int timeout_ms)
{
    return open_and_connect(host, port, timeout_ms);
}

void catalog_https_close(int handle)
{
    net_stream_close(handle);
}

int catalog_https_get_on(int handle, const char *host, const char *path,
                         uint8_t *out, uint32_t out_size, int timeout_ms)
{
    char req[CATALOG_TX_BUF_SIZE - CATALOG_NET_RING_HDR_SIZE - 2];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n", path, host);
    int ret = catalog_ring_write(g_tx_buf, (const uint8_t *)req, (uint16_t)req_len);
    if (ret < 0) {
        return ret;
    }
    ret = net_stream_tx_flush(handle);
    if (ret < 0) {
        return ret;
    }

    uint32_t hdr_len = 0;
    uint32_t body_len = 0;
    bool headers_done = false;
    bool disconnected = false;
    long content_length = -1;
    int64_t deadline = k_uptime_get() + timeout_ms;

    /* Same accumulation approach as catalog_https_get_once(), but completion
     * is driven by Content-Length once headers are parsed — NOT by waiting
     * for the peer to close, since closing would defeat the point of
     * keeping the connection alive for the next request. Falls back to
     * "wait for disconnect" only if Content-Length is absent. */
    while (k_uptime_get() < deadline && !disconnected) {
        struct net_event evt;
        int evt_type = net_stream_event_pop(&evt);
        if (evt_type == NET_EVT_DISCONNECTED) {
            disconnected = true;
        } else if (evt_type == NET_EVT_ERROR) {
            return -(int)evt.extra;
        }

        uint8_t chunk[1500];
        int n;
        while ((n = catalog_ring_read(g_rx_buf, chunk, sizeof(chunk))) > 0) {
            if (!headers_done) {
                uint32_t copy = (uint32_t)n;
                if (copy > CATALOG_HDR_BUF_SIZE - 1 - hdr_len) {
                    copy = CATALOG_HDR_BUF_SIZE - 1 - hdr_len;
                }
                memcpy(g_hdr_buf + hdr_len, chunk, copy);
                hdr_len += copy;
                g_hdr_buf[hdr_len] = '\0';

                char *sep = strstr((char *)g_hdr_buf, "\r\n\r\n");
                if (sep) {
                    headers_done = true;

                    const char *cl = find_header_value((char *)g_hdr_buf, "Content-Length:");
                    if (cl) {
                        content_length = strtol(cl, NULL, 10);
                    }

                    uint8_t *body_start = (uint8_t *)sep + 4;
                    uint32_t body_in_hdr = (uint32_t)((g_hdr_buf + hdr_len) - body_start);
                    if (body_in_hdr > out_size - body_len) {
                        body_in_hdr = out_size - body_len;
                    }
                    memcpy(out + body_len, body_start, body_in_hdr);
                    body_len += body_in_hdr;
                }
            } else {
                uint32_t copy = (uint32_t)n;
                if (copy > out_size - body_len) {
                    copy = out_size - body_len;
                }
                memcpy(out + body_len, chunk, copy);
                body_len += copy;
            }

            if (headers_done && content_length >= 0 &&
                body_len >= (uint32_t)content_length) {
                return (int)body_len;
            }
        }
        if (!disconnected) {
            k_sleep(K_MSEC(20));
        }
    }

    if (!headers_done && body_len == 0) {
        return -ETIMEDOUT;
    }
    return (int)body_len;
}
