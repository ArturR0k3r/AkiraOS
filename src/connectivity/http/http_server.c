/**
 * @file http_server.c
 * @brief HTTP Server Implementation for AkiraOS
 */

#include "http_server.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <stdio.h>
#ifdef CONFIG_AKIRA_HTTP_STATIC_FILES
#include <zephyr/fs/fs.h>
#include <strings.h> /* strcasecmp() for case-insensitive extension match */
#endif
#if defined(CONFIG_AKIRA_HTTP_WEBSOCKET)
#include <errno.h>
#include <strings.h>           /* strcasecmp */
#include <mbedtls/sha1.h>      /* mbedtls_sha1 (needs CONFIG_MBEDTLS_SHA1=y) */
#include <zephyr/sys/base64.h> /* base64_encode (needs CONFIG_BASE64=y) */
#include <lib/mem_helper.h>    /* AKIRA_BULK_BSS — PSRAM placement for the rx buffer */
#endif

LOG_MODULE_REGISTER(http_server, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define MAX_ROUTES 16
#define MAX_WS_CLIENTS 4
#define SERVER_THREAD_STACK_SIZE CONFIG_AKIRA_HTTP_SERVER_STACK_SIZE
#define SERVER_THREAD_PRIORITY 7

#define CORS_ALLOWED_METHODS "GET, POST, DELETE, OPTIONS"
#define CORS_ALLOWED_HEADERS "Content-Type, Authorization"
#define CORS_MAX_AGE "86400"

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    http_server_state_t state;
    http_server_stats_t stats;

    /* Routes */
    http_route_t routes[MAX_ROUTES];
    int route_count;

    /* Upload handler */
    const char *upload_path;
    upload_chunk_cb_t upload_cb;
    void *upload_user_data;

    /* WebSocket */
    bool ws_enabled;
    ws_message_cb_t ws_msg_cb;
    void *ws_msg_user_data;
    ws_event_cb_t ws_event_cb;
    void *ws_event_user_data;
    int ws_client_fds[MAX_WS_CLIENTS];

    /* Server socket */
    int server_fd;
    bool running;

    struct k_mutex mutex;
} http_srv;

/* Thread */
static K_THREAD_STACK_DEFINE(server_stack, SERVER_THREAD_STACK_SIZE);
static struct k_thread server_thread;

/*===========================================================================*/
/* HTTP Parsing Helpers                                                      */
/*===========================================================================*/

static const char *http_status_text(int code)
{
    switch (code)
    {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 500:
        return "Internal Server Error";
    default:
        return "Unknown";
    }
}

static const char *content_type_str(http_content_type_t type)
{
    switch (type)
    {
    case HTTP_CONTENT_HTML:
        return "text/html; charset=utf-8";
    case HTTP_CONTENT_JSON:
        return "application/json";
    case HTTP_CONTENT_TEXT:
        return "text/plain";
    case HTTP_CONTENT_BINARY:
        return "application/octet-stream";
    case HTTP_CONTENT_FORM:
        return "application/x-www-form-urlencoded";
    default:
        return "text/plain";
    }
}

static http_method_t parse_method(const char *method)
{
    if (strcmp(method, "GET") == 0)
        return HTTP_GET;
    if (strcmp(method, "POST") == 0)
        return HTTP_POST;
    if (strcmp(method, "PUT") == 0)
        return HTTP_PUT;
    if (strcmp(method, "DELETE") == 0)
        return HTTP_DELETE;
    if (strcmp(method, "OPTIONS") == 0)
        return HTTP_OPTIONS;
    if (strcmp(method, "PATCH") == 0)
        return HTTP_PATCH;
    return HTTP_GET;
}

static bool path_matches(const char *pattern, const char *path)
{
    /* Simple wildcard matching */
    while (*pattern && *path)
    {
        if (*pattern == '*')
        {
            return true; /* Wildcard matches rest */
        }
        if (*pattern != *path)
        {
            return false;
        }
        pattern++;
        path++;
    }

    return (*pattern == *path) || (*pattern == '*');
}

/*===========================================================================*/
/* Response Implementation                                                   */
/*===========================================================================*/

typedef struct
{
    int client_fd;
    bool headers_sent;
    int status_code;
    http_content_type_t content_type;
    char headers[512];
    size_t headers_len;
    char allowed_origin[256];
    bool is_preflight;
} response_ctx_t;

static response_ctx_t *current_resp_ctx = NULL;

static int resp_set_header(const char *name, const char *value)
{
    if (!current_resp_ctx || !name || !value)
        return -1;

    /* Append header: "Name: Value\r\n" */
    int avail = sizeof(current_resp_ctx->headers) - current_resp_ctx->headers_len;
    if (avail <= 1)
        return -1;

    int written = snprintf(current_resp_ctx->headers + current_resp_ctx->headers_len,
                           avail, "%s: %s\r\n", name, value);
    if (written < 0 || written >= avail)
        return -1;

    current_resp_ctx->headers_len += written;
    return 0;
}

static int resp_send(response_ctx_t *ctx, const char *data, size_t len)
{
    if (!ctx->headers_sent)
    {
        char header[1024];
        int pos = 0;

        pos += snprintf(header + pos, sizeof(header) - pos,
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n",
                        ctx->status_code, http_status_text(ctx->status_code),
                        content_type_str(ctx->content_type), len);

        /* Custom headers added by handlers */
        if (ctx->headers_len > 0)
        {
            int copy_len = ctx->headers_len;
            if (pos + copy_len < (int)sizeof(header))
            {
                memcpy(header + pos, ctx->headers, copy_len);
                pos += copy_len;
            }
        }

        /* Dynamic CORS: echo back Origin when allowed */
        if (ctx->allowed_origin[0] != '\0')
        {
            pos += snprintf(header + pos, sizeof(header) - pos,
                            "Access-Control-Allow-Origin: %s\r\n",
                            ctx->allowed_origin);
            pos += snprintf(header + pos, sizeof(header) - pos,
                            "Vary: Origin\r\n");

            if (ctx->is_preflight)
            {
                pos += snprintf(header + pos, sizeof(header) - pos,
                                "Access-Control-Allow-Methods: " CORS_ALLOWED_METHODS "\r\n");
                pos += snprintf(header + pos, sizeof(header) - pos,
                                "Access-Control-Allow-Headers: " CORS_ALLOWED_HEADERS "\r\n");
                pos += snprintf(header + pos, sizeof(header) - pos,
                                "Access-Control-Max-Age: " CORS_MAX_AGE "\r\n");
            }
        }

        /* End headers */
        pos += snprintf(header + pos, sizeof(header) - pos, "\r\n");

        send(ctx->client_fd, header, pos, 0);
        ctx->headers_sent = true;
        http_srv.stats.bytes_sent += pos;
    }

    if (data && len > 0)
    {
        send(ctx->client_fd, data, len, 0);
        http_srv.stats.bytes_sent += len;
    }

    return 0;
}

static int __attribute__((unused)) resp_send_json(response_ctx_t *ctx, const char *json)
{
    ctx->content_type = HTTP_CONTENT_JSON;
    return resp_send(ctx, json, strlen(json));
}

/* Helper: simple header extraction (case-sensitive for typical clients) */
static void extract_header_value(const char *raw, const char *header, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!raw || !header || !out)
        return;

    const char *p = strstr(raw, header);
    if (!p)
    {
        /* try with CRLF prefix */
        char with_crlf[64];
        snprintf(with_crlf, sizeof(with_crlf), "\r\n%s", header);
        p = strstr(raw, with_crlf);
        if (p)
            p += 2; /* skip CRLF */
    }
    if (!p)
        return;

    /* Move past header name */
    p += strlen(header);

    /* Skip optional spaces */
    while (*p == ' ' || *p == '\t')
        p++;

    /* Value ends at CRLF or LF */
    const char *end = strstr(p, "\r\n");
    if (!end)
        end = strchr(p, '\n');
    size_t l = end ? (size_t)(end - p) : strlen(p);
    if (l >= out_len)
        l = out_len - 1;
    memcpy(out, p, l);
    out[l] = '\0';
}

static bool host_ends_with(const char *host, const char *suffix)
{
    if (!host || !suffix)
        return false;
    size_t hlen = strlen(host);
    size_t slen = strlen(suffix);
    if (hlen < slen)
        return false;
    return strcmp(host + hlen - slen, suffix) == 0;
}

static bool origin_allowed_by_policy(const char *origin)
{
    if (!origin || origin[0] == '\0')
        return false;

    /* Extract host portion from origin (skip scheme) */
    const char *p = strstr(origin, "://");
    const char *hoststart = p ? p + 3 : origin;
    const char *hostend = strchr(hoststart, '/');
    size_t hostport_len = hostend ? (size_t)(hostend - hoststart) : strlen(hoststart);
    char hostport[128] = {0};
    if (hostport_len >= sizeof(hostport))
        hostport_len = sizeof(hostport) - 1;
    memcpy(hostport, hoststart, hostport_len);
    hostport[hostport_len] = '\0';

    /* Split host:port */
    char host[128] = {0};
    char *colon = strchr(hostport, ':');
    size_t host_len = colon ? (size_t)(colon - hostport) : strlen(hostport);
    if (host_len >= sizeof(host))
        host_len = sizeof(host) - 1;
    memcpy(host, hostport, host_len);
    host[host_len] = '\0';

    /* Allow localhost and loopback */
    if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0)
        return true;

    /* Allow any subdomain of akiraos.dev */
    if (strcmp(host, "akiraos.dev") == 0 || host_ends_with(host, ".akiraos.dev"))
        return true;

    return false;
}

/* Wrappers so handlers can call res->send/res->send_json without ctx param */
static int res_send_wrapper(const char *data, size_t len)
{
    if (!current_resp_ctx)
        return -1;
    return resp_send(current_resp_ctx, data, len);
}

static int res_send_json_wrapper(const char *json)
{
    if (!current_resp_ctx)
        return -1;
    current_resp_ctx->content_type = HTTP_CONTENT_JSON;
    return resp_send(current_resp_ctx, json, strlen(json));
}

#ifdef CONFIG_AKIRA_HTTP_STATIC_FILES
/*===========================================================================*/
/* Static File Serving                                                       */
/*===========================================================================*/

#define STATIC_DIR_MAX 128    /* max length of the configured static root  */
#define STATIC_PATH_MAX 320   /* root + request path + "/index.html"        */
#define STATIC_CHUNK_SIZE 512 /* file streaming buffer (stack)              */

/* Configured filesystem directory served for unmatched GET requests.
 * Empty string means static serving is disabled. */
static char http_static_dir[STATIC_DIR_MAX];

/* Map a file's extension to an HTTP Content-Type. */
static const char *static_mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot || dot[1] == '\0')
    {
        return "application/octet-stream";
    }

    dot++; /* skip the '.' */
    if (strcasecmp(dot, "html") == 0)
        return "text/html; charset=utf-8";
    if (strcasecmp(dot, "css") == 0)
        return "text/css";
    if (strcasecmp(dot, "js") == 0)
        return "application/javascript";
    if (strcasecmp(dot, "json") == 0)
        return "application/json";
    if (strcasecmp(dot, "png") == 0)
        return "image/png";
    if (strcasecmp(dot, "svg") == 0)
        return "image/svg+xml";
    if (strcasecmp(dot, "txt") == 0)
        return "text/plain; charset=utf-8";

    return "application/octet-stream";
}

/* Try to serve @p path as a static file under the configured directory.
 *
 * Returns true when a response was fully written to the socket (the file was
 * streamed, or a 403 was sent for an unsafe path). Returns false when the
 * request was not handled here so the caller can emit the generic 404.
 *
 * Security: request paths must be absolute ("/...") and must not contain a
 * ".." sequence, blocking directory traversal outside the static root. */
static bool try_serve_static(response_ctx_t *ctx, const char *path)
{
    if (http_static_dir[0] == '\0')
    {
        return false;
    }

    if (path[0] != '/' || strstr(path, "..") != NULL)
    {
        ctx->status_code = 403;
        ctx->content_type = HTTP_CONTENT_TEXT;
        resp_send(ctx, "Forbidden", 9);
        return true;
    }

    /* Compose the on-disk path. Directory-style paths (ending in '/', which
     * includes the root "/") map to index.html. */
    char full[STATIC_PATH_MAX];
    int n;
    size_t plen = strlen(path);
    if (path[plen - 1] == '/')
    {
        n = snprintf(full, sizeof(full), "%s%sindex.html", http_static_dir, path);
    }
    else
    {
        n = snprintf(full, sizeof(full), "%s%s", http_static_dir, path);
    }
    if (n < 0 || n >= (int)sizeof(full))
    {
        return false;
    }

    struct fs_dirent entry;
    if (fs_stat(full, &entry) != 0 || entry.type != FS_DIR_ENTRY_FILE)
    {
        return false; /* fall through to generic 404 */
    }

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, full, FS_O_READ) < 0)
    {
        return false;
    }

    char header[512];
    int pos = snprintf(header, sizeof(header),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %zu\r\n"
                       "Connection: close\r\n",
                       static_mime_type(full), entry.size);

    if (pos > 0 && pos < (int)sizeof(header) && ctx->allowed_origin[0] != '\0')
    {
        pos += snprintf(header + pos, sizeof(header) - pos,
                        "Access-Control-Allow-Origin: %s\r\n"
                        "Vary: Origin\r\n",
                        ctx->allowed_origin);
    }
    if (pos > 0 && pos < (int)sizeof(header))
    {
        pos += snprintf(header + pos, sizeof(header) - pos, "\r\n");
    }
    if (pos <= 0 || pos >= (int)sizeof(header))
    {
        fs_close(&file);
        return false;
    }

    send(ctx->client_fd, header, pos, 0);
    ctx->headers_sent = true;
    ctx->status_code = 200;
    http_srv.stats.bytes_sent += pos;

    char buf[STATIC_CHUNK_SIZE];
    ssize_t rd;
    while ((rd = fs_read(&file, buf, sizeof(buf))) > 0)
    {
        send(ctx->client_fd, buf, rd, 0);
        http_srv.stats.bytes_sent += rd;
    }

    fs_close(&file);
    LOG_INF("Served static file: %s (%zu bytes)", full, entry.size);
    return true;
}
#endif /* CONFIG_AKIRA_HTTP_STATIC_FILES */

/*===========================================================================*/
/* WebSocket Protocol (RFC 6455)                                             */
/*===========================================================================*/

#if defined(CONFIG_AKIRA_HTTP_WEBSOCKET)

/* Magic GUID appended to the client key before hashing (RFC 6455 s1.3). */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* Frame opcodes (RFC 6455 s5.2). */
#define WS_OP_CONT  0x0
#define WS_OP_TEXT  0x1
#define WS_OP_BIN   0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING  0x9
#define WS_OP_PONG  0xA

/* Send exactly @len bytes, looping over partial TCP writes. */
static int ws_write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            return -EIO;
        off += (size_t)n;
        http_srv.stats.bytes_sent += (size_t)n;
    }
    return 0;
}

/* Receive exactly @len bytes, looping over short reads. */
static int ws_read_all(int fd, uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = recv(fd, buf + off, len - off, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            return -ENOTCONN; /* peer closed the connection */
        off += (size_t)n;
        http_srv.stats.bytes_received += (size_t)n;
    }
    return 0;
}

/* Compute Sec-WebSocket-Accept = base64(SHA1(key + GUID)).
 * @out must hold at least 29 bytes; the result is NUL-terminated. */
static int ws_compute_accept(const char *key, char *out, size_t out_len)
{
    char concat[64];
    unsigned char digest[20];
    size_t olen = 0;

    /* key is <=24 base64 chars, GUID is 36 -> 60 chars, fits concat[64]. */
    int n = snprintf(concat, sizeof(concat), "%s%s", key, WS_GUID);
    if (n < 0 || n >= (int)sizeof(concat))
        return -EINVAL;

    if (mbedtls_sha1((const unsigned char *)concat, (size_t)n, digest) != 0)
        return -EIO;

    /* base64_encode writes a trailing NUL and needs dlen >= 28+1. */
    if (base64_encode((uint8_t *)out, out_len, &olen, digest, sizeof(digest)) != 0)
        return -ENOMEM;

    return 0;
}

/* Encode and transmit one unfragmented, unmasked server->client frame. */
static int ws_send_frame(int fd, uint8_t opcode, const uint8_t *data, size_t len)
{
    uint8_t hdr[10];
    size_t hlen;

    hdr[0] = 0x80 | (opcode & 0x0F); /* FIN=1, RSV=0, opcode */

    if (len < 126)
    {
        hdr[1] = (uint8_t)len; /* MASK=0 for server frames */
        hlen = 2;
    }
    else if (len <= 0xFFFF)
    {
        hdr[1] = 126;
        hdr[2] = (uint8_t)((len >> 8) & 0xFF);
        hdr[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    }
    else
    {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++)
            hdr[2 + i] = (uint8_t)(((uint64_t)len >> (56 - 8 * i)) & 0xFF);
        hlen = 10;
    }

    int ret = ws_write_all(fd, hdr, hlen);
    if (ret != 0)
        return ret;

    if (data && len > 0)
        return ws_write_all(fd, data, len);

    return 0;
}

/* Send @data as a frame to one client id, or to all clients when id == -1. */
static int ws_send_to_client(int client_id, uint8_t opcode,
                             const uint8_t *data, size_t len)
{
    if (!http_srv.ws_enabled)
        return -ENOTSUP;

    if (client_id == -1)
    {
        int sent = 0;
        int last_err = 0;
        k_mutex_lock(&http_srv.mutex, K_FOREVER);
        for (int i = 0; i < MAX_WS_CLIENTS; i++)
        {
            int fd = http_srv.ws_client_fds[i];
            if (fd >= 0)
            {
                int r = ws_send_frame(fd, opcode, data, len);
                if (r == 0)
                    sent++;
                else
                    last_err = r;
            }
        }
        k_mutex_unlock(&http_srv.mutex);
        if (sent > 0)
            return 0;
        return last_err ? last_err : -ENOTCONN;
    }

    if (client_id < 0 || client_id >= MAX_WS_CLIENTS)
        return -EINVAL;

    k_mutex_lock(&http_srv.mutex, K_FOREVER);
    int fd = http_srv.ws_client_fds[client_id];
    k_mutex_unlock(&http_srv.mutex);

    if (fd < 0)
        return -ENOTCONN;

    return ws_send_frame(fd, opcode, data, len);
}

/* Blocking receive loop: decode client frames, remove the mask, dispatch data
 * frames and handle ping/pong/close until the peer closes or a protocol error
 * occurs. The fd is left OPEN for the caller (accept loop) to close. */
static void ws_serve_client(int client_fd, int client_id)
{
    /* The single accept-loop thread services WS sessions one at a time, so a
     * shared static reassembly buffer is safe and keeps the stack small. Placed
     * in PSRAM (.ext_ram.bss) on boards that have it — it is only touched on the
     * slow WS receive path, never on a hot path, and keeping it out of scarce
     * internal DRAM lets the flagship board fit. No-op on non-PSRAM targets. */
    static uint8_t payload[CONFIG_AKIRA_HTTP_WS_MAX_PAYLOAD] AKIRA_BULK_BSS;

    for (;;)
    {
        uint8_t h[2];
        if (ws_read_all(client_fd, h, sizeof(h)) != 0)
            return;

        uint8_t opcode = h[0] & 0x0F;
        bool masked = (h[1] & 0x80) != 0;
        uint64_t plen = h[1] & 0x7F;

        if (plen == 126)
        {
            uint8_t ext[2];
            if (ws_read_all(client_fd, ext, sizeof(ext)) != 0)
                return;
            plen = ((uint64_t)ext[0] << 8) | ext[1];
        }
        else if (plen == 127)
        {
            uint8_t ext[8];
            if (ws_read_all(client_fd, ext, sizeof(ext)) != 0)
                return;
            plen = 0;
            for (int i = 0; i < 8; i++)
                plen = (plen << 8) | ext[i];
        }

        /* RFC 6455 s5.1: every client->server frame MUST be masked. */
        if (!masked)
        {
            uint8_t code[2] = {0x03, 0xEA}; /* 1002 protocol error */
            ws_send_frame(client_fd, WS_OP_CLOSE, code, sizeof(code));
            return;
        }

        uint8_t mask[4];
        if (ws_read_all(client_fd, mask, sizeof(mask)) != 0)
            return;

        if (plen > sizeof(payload))
        {
            LOG_WRN("WS frame too large (%llu bytes)", (unsigned long long)plen);
            uint8_t code[2] = {0x03, 0xF1}; /* 1009 message too big */
            ws_send_frame(client_fd, WS_OP_CLOSE, code, sizeof(code));
            return;
        }

        if (plen > 0)
        {
            if (ws_read_all(client_fd, payload, (size_t)plen) != 0)
                return;
            for (uint64_t i = 0; i < plen; i++)
                payload[i] ^= mask[i & 3];
        }

        switch (opcode)
        {
        case WS_OP_CONT:
        case WS_OP_TEXT:
        case WS_OP_BIN:
            /* Deliver data frames to the application. Fragmented messages are
             * passed through fragment-by-fragment (no reassembly). */
            if (http_srv.ws_msg_cb)
                http_srv.ws_msg_cb(client_id, payload, (size_t)plen,
                                   http_srv.ws_msg_user_data);
            break;

        case WS_OP_PING:
            ws_send_frame(client_fd, WS_OP_PONG, payload, (size_t)plen);
            break;

        case WS_OP_PONG:
            /* keepalive response - ignore */
            break;

        case WS_OP_CLOSE:
            /* Echo the close frame to complete the closing handshake. */
            ws_send_frame(client_fd, WS_OP_CLOSE, payload, (size_t)plen);
            return;

        default:
        {
            uint8_t code[2] = {0x03, 0xEA}; /* 1002 protocol error */
            ws_send_frame(client_fd, WS_OP_CLOSE, code, sizeof(code));
            return;
        }
        }
    }
}

/* Perform the RFC 6455 opening handshake on @client_fd using the already-read
 * request text in @raw, then service the connection until it closes. The fd is
 * closed by the accept loop (server_thread_fn) after handle_request returns. */
static void ws_handle_upgrade(int client_fd, const char *raw)
{
    char key[32] = {0};
    extract_header_value(raw, "Sec-WebSocket-Key:", key, sizeof(key));
    if (key[0] == '\0')
    {
        const char *bad = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        send(client_fd, bad, strlen(bad), 0);
        return;
    }

    char accept[32] = {0};
    if (ws_compute_accept(key, accept, sizeof(accept)) != 0)
    {
        const char *err = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
        send(client_fd, err, strlen(err), 0);
        return;
    }

    /* Reserve a client slot in the shared fd table. */
    int client_id = -1;
    k_mutex_lock(&http_srv.mutex, K_FOREVER);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
        if (http_srv.ws_client_fds[i] < 0)
        {
            http_srv.ws_client_fds[i] = client_fd;
            client_id = i;
            break;
        }
    }
    k_mutex_unlock(&http_srv.mutex);

    if (client_id < 0)
    {
        const char *full = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n";
        send(client_fd, full, strlen(full), 0);
        return;
    }

    char resp[192];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept);
    if (n < 0 || n >= (int)sizeof(resp) ||
        ws_write_all(client_fd, (const uint8_t *)resp, (size_t)n) != 0)
    {
        k_mutex_lock(&http_srv.mutex, K_FOREVER);
        http_srv.ws_client_fds[client_id] = -1;
        k_mutex_unlock(&http_srv.mutex);
        return;
    }

    LOG_INF("WebSocket client %d connected", client_id);
    if (http_srv.ws_event_cb)
        http_srv.ws_event_cb(client_id, true, http_srv.ws_event_user_data);

    /* Blocks this accept-loop iteration until the client disconnects. */
    ws_serve_client(client_fd, client_id);

    if (http_srv.ws_event_cb)
        http_srv.ws_event_cb(client_id, false, http_srv.ws_event_user_data);

    k_mutex_lock(&http_srv.mutex, K_FOREVER);
    if (http_srv.ws_client_fds[client_id] == client_fd)
        http_srv.ws_client_fds[client_id] = -1;
    k_mutex_unlock(&http_srv.mutex);

    LOG_INF("WebSocket client %d disconnected", client_id);
}

#endif /* CONFIG_AKIRA_HTTP_WEBSOCKET */


/*===========================================================================*/
/* Request Handling                                                          */
/*===========================================================================*/

static http_route_t *find_route(http_method_t method, const char *path)
{
    for (int i = 0; i < http_srv.route_count; i++)
    {
        if (http_srv.routes[i].method == method &&
            path_matches(http_srv.routes[i].path, path))
        {
            return &http_srv.routes[i];
        }
    }
    return NULL;
}

static int handle_request(int client_fd, char *buffer, size_t len)
{
    /* Parse request line */
    char method_str[8] = {0};
    char path[128] = {0};
    char version[16] = {0};

    if (sscanf(buffer, "%7s %127s %15s", method_str, path, version) != 3)
    {
        LOG_WRN("Invalid HTTP request");
        return -1;
    }

    http_method_t method = parse_method(method_str);

    /* Split path and query */
    char *query = strchr(path, '?');
    if (query)
    {
        *query = '\0';
        query++;
    }

    /* Find body */
    char *body = strstr(buffer, "\r\n\r\n");
    size_t body_len = 0;
    if (body)
    {
        body += 4;
        body_len = len - (body - buffer);
    }

    /* Find Content-Length */
    size_t content_length = 0;
    char *cl = strstr(buffer, "Content-Length:");
    if (cl)
    {
        content_length = atoi(cl + 15);
    }

    LOG_INF("HTTP %s %s", method_str, path);

#if defined(CONFIG_AKIRA_HTTP_WEBSOCKET)
    /* Intercept a WebSocket opening handshake before normal route dispatch.
     * A valid request is: GET + "Upgrade: websocket". ws_handle_upgrade()
     * runs the entire session (handshake + frame loop) inline; the accept
     * loop closes client_fd after we return. */
    if (http_srv.ws_enabled && method == HTTP_GET)
    {
        char upgrade_hdr[24] = {0};
        extract_header_value(buffer, "Upgrade:", upgrade_hdr, sizeof(upgrade_hdr));
        if (strcasecmp(upgrade_hdr, "websocket") == 0)
        {
            ws_handle_upgrade(client_fd, buffer);
            http_srv.stats.requests_handled++;
            return 0;
        }
    }
#endif

    /* Build request struct */
    http_request_t req = {
        .method = method,
        .path = path,
        .query = query,
        .body = body,
        .body_len = body_len,
        .content_length = content_length,
        .raw = buffer,
        .client_fd = client_fd,
    };

    /* Build response context */
    response_ctx_t resp_ctx = {
        .client_fd = client_fd,
        .headers_sent = false,
        .status_code = 200,
        .content_type = HTTP_CONTENT_HTML,
    };

    http_response_t res = {
        .status_code = 200,
        .content_type = HTTP_CONTENT_HTML,
        .body = NULL,
        .body_len = 0,
    };

    /* initialize response ctx extras */
    resp_ctx.headers_len = 0;
    resp_ctx.allowed_origin[0] = '\0';
    resp_ctx.is_preflight = (method == HTTP_OPTIONS);

    /* wire response helpers so handlers can set headers/send easily */
    res.set_header = resp_set_header;
    res.send = res_send_wrapper;
    res.send_json = res_send_json_wrapper;

    /* Compute allowed origin from request headers */
    char origin_val[256] = {0};
    extract_header_value(buffer, "Origin:", origin_val, sizeof(origin_val));
    if (origin_val[0] != '\0' && origin_allowed_by_policy(origin_val))
    {
        strncpy(resp_ctx.allowed_origin, origin_val, sizeof(resp_ctx.allowed_origin) - 1);
        resp_ctx.allowed_origin[sizeof(resp_ctx.allowed_origin) - 1] = '\0';
    }

    /* make current context available to helpers */
    current_resp_ctx = &resp_ctx;

    /* Find matching route */
    http_route_t *route = find_route(method, path);

    if (route)
    {
        int ret = route->handler(&req, &res, route->user_data);
        resp_ctx.status_code = res.status_code;
        resp_ctx.content_type = res.content_type;

        if (!resp_ctx.headers_sent)
        {
            if (ret == 0 && res.body != NULL)
            {
                size_t blen = res.body_len ? res.body_len : strlen(res.body);
                resp_send(&resp_ctx, res.body, blen);
            }
            else if (ret != 0)
            {
                resp_ctx.status_code = 500;
                resp_send(&resp_ctx, "Internal Server Error", 21);
            }
            else
            {
                /* Handler sent no body and no error: send empty 200 */
                resp_send(&resp_ctx, "", 0);
            }
        }
    }
    else
    {
        /* Check for upload path */
        if (http_srv.upload_path && http_srv.upload_cb &&
            strcmp(path, http_srv.upload_path) == 0 && method == HTTP_POST)
        {

            /* Stream upload */
            if (body && body_len > 0)
            {
                http_srv.upload_cb((uint8_t *)body, body_len, 0,
                                   content_length, http_srv.upload_user_data);
            }

            resp_ctx.status_code = 200;
            resp_ctx.content_type = HTTP_CONTENT_JSON;
            resp_send(&resp_ctx, "{\"status\":\"ok\"}", 15);
        }
        else
        {
#ifdef CONFIG_AKIRA_HTTP_STATIC_FILES
            if (method == HTTP_GET && try_serve_static(&resp_ctx, path))
            {
                /* Response was streamed by the static file server. */
            }
            else
#endif
            {
                /* 404 Not Found */
                resp_ctx.status_code = 404;
                resp_send(&resp_ctx, "Not Found", 9);
            }
        }
    }

    http_srv.stats.requests_handled++;

    /* clear current response context */
    current_resp_ctx = NULL;

    return 0;
}

/*===========================================================================*/
/* Server Thread                                                             */
/*===========================================================================*/

static void server_thread_fn(void *p1, void *p2, void *p3)
{
    struct sockaddr_in addr;
    int ret;

    /* Create socket */
    http_srv.server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (http_srv.server_fd < 0)
    {
        LOG_ERR("Failed to create socket: %d", errno);
        http_srv.state = HTTP_SERVER_ERROR;
        return;
    }

    /* Allow address reuse */
    int optval = 1;
    setsockopt(http_srv.server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    /* Bind */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_SERVER_PORT);

    ret = bind(http_srv.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0)
    {
        LOG_ERR("Failed to bind socket: %d", errno);
        close(http_srv.server_fd);
        http_srv.state = HTTP_SERVER_ERROR;
        return;
    }

    /* Listen */
    ret = listen(http_srv.server_fd, 4);
    if (ret < 0)
    {
        LOG_ERR("Failed to listen: %d", errno);
        close(http_srv.server_fd);
        http_srv.state = HTTP_SERVER_ERROR;
        return;
    }

    LOG_INF("HTTP server listening on port %d", HTTP_SERVER_PORT);
    http_srv.state = HTTP_SERVER_RUNNING;
    http_srv.running = true;

    /* Accept loop */
    while (http_srv.running)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(http_srv.server_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);

        if (client_fd < 0)
        {
            if (http_srv.running)
            {
                LOG_WRN("Accept failed: %d", errno);
            }
            continue;
        }

        http_srv.stats.active_connections++;

        /* Read request */
        char buffer[HTTP_BUFFER_SIZE];
        ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (received > 0)
        {
            buffer[received] = '\0';
            http_srv.stats.bytes_received += received;
            handle_request(client_fd, buffer, received);
        }

        close(client_fd);
        http_srv.stats.active_connections--;
    }

    close(http_srv.server_fd);
    http_srv.state = HTTP_SERVER_STOPPED;
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int akira_http_server_init(void)
{
    if (http_srv.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing HTTP server");

    k_mutex_init(&http_srv.mutex);
    memset(&http_srv.stats, 0, sizeof(http_server_stats_t));
    memset(http_srv.ws_client_fds, -1, sizeof(http_srv.ws_client_fds));

    http_srv.state = HTTP_SERVER_STOPPED;
    http_srv.initialized = true;

    return 0;
}

int akira_http_server_start(void)
{
    if (!http_srv.initialized)
    {
        return -EINVAL;
    }

    if (http_srv.state == HTTP_SERVER_RUNNING)
    {
        return 0;
    }

    http_srv.state = HTTP_SERVER_STARTING;

    k_thread_create(&server_thread, server_stack,
                    K_THREAD_STACK_SIZEOF(server_stack),
                    server_thread_fn,
                    NULL, NULL, NULL,
                    SERVER_THREAD_PRIORITY, 0, K_NO_WAIT);

    k_thread_name_set(&server_thread, "http_server");

    LOG_INF("HTTP server starting...");
    return 0;
}

int akira_http_server_stop(void)
{
    if (http_srv.state != HTTP_SERVER_RUNNING)
    {
        return 0;
    }

    http_srv.running = false;

    /* Close server socket to unblock accept() */
    if (http_srv.server_fd >= 0)
    {
        close(http_srv.server_fd);
        http_srv.server_fd = -1;
    }

    /* Wait for thread to exit */
    k_thread_join(&server_thread, K_SECONDS(5));

    http_srv.state = HTTP_SERVER_STOPPED;
    LOG_INF("HTTP server stopped");

    return 0;
}

http_server_state_t akira_http_server_get_state(void)
{
    return http_srv.state;
}

bool akira_http_server_is_running(void)
{
    return http_srv.state == HTTP_SERVER_RUNNING;
}

int akira_http_register_route(const http_route_t *route)
{
    if (!route || !route->path || !route->handler)
    {
        return -EINVAL;
    }

    k_mutex_lock(&http_srv.mutex, K_FOREVER);

    if (http_srv.route_count >= MAX_ROUTES)
    {
        k_mutex_unlock(&http_srv.mutex);
        return -ENOMEM;
    }

    memcpy(&http_srv.routes[http_srv.route_count], route, sizeof(http_route_t));
    http_srv.route_count++;

    k_mutex_unlock(&http_srv.mutex);

    LOG_INF("Registered route: %s", route->path);
    return 0;
}

int akira_http_unregister_route(http_method_t method, const char *path)
{
    k_mutex_lock(&http_srv.mutex, K_FOREVER);

    for (int i = 0; i < http_srv.route_count; i++)
    {
        if (http_srv.routes[i].method == method &&
            strcmp(http_srv.routes[i].path, path) == 0)
        {
            /* Shift remaining routes */
            for (int j = i; j < http_srv.route_count - 1; j++)
            {
                http_srv.routes[j] = http_srv.routes[j + 1];
            }
            http_srv.route_count--;
            k_mutex_unlock(&http_srv.mutex);
            return 0;
        }
    }

    k_mutex_unlock(&http_srv.mutex);
    return -ENOENT;
}

int akira_http_register_upload_handler(const char *path, upload_chunk_cb_t callback,
                                       void *user_data)
{
    http_srv.upload_path = path;
    http_srv.upload_cb = callback;
    http_srv.upload_user_data = user_data;
    return 0;
}

int akira_http_set_static_dir(const char *path)
{
#ifdef CONFIG_AKIRA_HTTP_STATIC_FILES
    if (!path || path[0] == '\0')
    {
        return -EINVAL;
    }

    size_t len = strlen(path);
    if (len >= sizeof(http_static_dir))
    {
        return -ENAMETOOLONG;
    }

    /* Store the root, dropping a single trailing '/' so paths (which always
     * begin with '/') can be joined uniformly. Keep a lone "/" as-is. */
    strncpy(http_static_dir, path, sizeof(http_static_dir) - 1);
    http_static_dir[sizeof(http_static_dir) - 1] = '\0';
    if (len > 1 && http_static_dir[len - 1] == '/')
    {
        http_static_dir[len - 1] = '\0';
    }

    LOG_INF("Static file directory set: %s", http_static_dir);
    return 0;
#else
    ARG_UNUSED(path);
    /* Static file serving is not implemented. Return -ENOSYS rather than 0 so
     * a caller does not believe a static directory is being served. */
    return -ENOSYS;
#endif
}

void akira_http_notify_network(bool connected, const char *ip_address)
{
    if (connected && ip_address)
    {
        strncpy(http_srv.stats.server_ip, ip_address, sizeof(http_srv.stats.server_ip) - 1);
        LOG_INF("Network connected: %s", ip_address);

#ifdef CONFIG_AKIRA_MDNS
        /* Advertise via mDNS/DNS-SD once we have an IP */
        extern void akira_mdns_start(const char *device_name);
        akira_mdns_start(NULL);
#endif
    }
    else
    {
        http_srv.stats.server_ip[0] = '\0';
        LOG_INF("Network disconnected");
    }
}

int akira_http_get_stats(http_server_stats_t *stats)
{
    if (!stats)
    {
        return -EINVAL;
    }

    k_mutex_lock(&http_srv.mutex, K_FOREVER);
    memcpy(stats, &http_srv.stats, sizeof(http_server_stats_t));
    stats->state = http_srv.state;
    stats->ws_clients = akira_http_ws_client_count();
    k_mutex_unlock(&http_srv.mutex);

    return 0;
}

/* Upload response: set by upload_chunk_cb_t on final chunk; read by handle_request */
static const char *upload_response_ptr = "{\"status\":\"ok\"}";

void akira_http_set_upload_response(const char *json)
{
    if (json)
    {
        upload_response_ptr = json;
    }
}

/*===========================================================================*/
/* WebSocket API                                                             */
/*===========================================================================*/

int akira_http_enable_websocket(void)
{
    http_srv.ws_enabled = true;
    LOG_INF("WebSocket support enabled");
    return 0;
}

int akira_http_ws_register_message_cb(ws_message_cb_t callback, void *user_data)
{
    http_srv.ws_msg_cb = callback;
    http_srv.ws_msg_user_data = user_data;
    return 0;
}

int akira_http_ws_register_event_cb(ws_event_cb_t callback, void *user_data)
{
    http_srv.ws_event_cb = callback;
    http_srv.ws_event_user_data = user_data;
    return 0;
}

int akira_http_ws_send(int client_id, const uint8_t *data, size_t len)
{
#if defined(CONFIG_AKIRA_HTTP_WEBSOCKET)
    /* Raw byte payloads are sent as binary frames (opcode 0x2). */
    return ws_send_to_client(client_id, WS_OP_BIN, data, len);
#else
    ARG_UNUSED(client_id);
    ARG_UNUSED(data);
    ARG_UNUSED(len);
    return -ENOTSUP;
#endif
}

int akira_http_ws_send_text(int client_id, const char *text)
{
    if (!text)
        return -EINVAL;
#if defined(CONFIG_AKIRA_HTTP_WEBSOCKET)
    /* Text frames (opcode 0x1) carry UTF-8 payloads. */
    return ws_send_to_client(client_id, WS_OP_TEXT,
                             (const uint8_t *)text, strlen(text));
#else
    ARG_UNUSED(client_id);
    return -ENOTSUP;
#endif
}

int akira_http_ws_disconnect(int client_id)
{
    if (client_id < 0 || client_id >= MAX_WS_CLIENTS)
    {
        return -EINVAL;
    }

    if (http_srv.ws_client_fds[client_id] >= 0)
    {
        close(http_srv.ws_client_fds[client_id]);
        http_srv.ws_client_fds[client_id] = -1;
    }

    return 0;
}

int akira_http_ws_client_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
        if (http_srv.ws_client_fds[i] >= 0)
        {
            count++;
        }
    }
    return count;
}
