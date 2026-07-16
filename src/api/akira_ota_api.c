/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_ota_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ota_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_ota_api.c
 * @brief OTA update WASM native API implementation.
 *
 * Security:
 *   - Only HTTPS URLs are accepted (http:// rejected with -EPROTO).
 *   - The manifest URL is validated before any network operation.
 *   - WASM sandbox cannot write flash directly — only via this native API.
 *   - Requires AKIRA_CAP_OTA_TRIGGER on the calling app.
 *
 * Gate: CONFIG_AKIRA_WASM_OTA=y
 */

#ifdef CONFIG_AKIRA_WASM_OTA

#include "akira_ota_api.h"
#include <runtime/security.h>
#include <runtime/akira_runtime.h>
#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <connectivity/ota/ota_manager.h>
#endif
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

#ifdef CONFIG_AKIRA_BOOT_GUARD
#include <akira_boot_guard.h>
#endif

#ifdef CONFIG_AKIRA_TELEMETRY
#include <lib/akira_telemetry.h>
#endif

#include <akira.h>   /* AKIRA_VERSION_MAJOR/MINOR/PATCH */

#if defined(CONFIG_HTTP_CLIENT) && defined(CONFIG_NET_SOCKETS)
#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/http/method.h>
#include <zephyr/net/http/parser_url.h>
#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
#include <zephyr/net/tls_credentials.h>
#endif
#endif /* CONFIG_HTTP_CLIENT && CONFIG_NET_SOCKETS */

#define MANIFEST_URL_MAX 256

/* ── URL security check ─────────────────────────────────────────────────── */

static int validate_https_url(const char *url)
{
    if (!url || url[0] == '\0') {
        return -EINVAL;
    }
    if (strncmp(url, "https://", 8) != 0) {
        LOG_WRN("OTA API: only HTTPS URLs allowed (got '%.*s...')", 16, url);
        return -EPROTO;
    }
    return 0;
}

/* ── Real OTA download implementation ───────────────────────────────────── */
/*
 * Manifest fetch + firmware streaming need Zephyr's HTTP client and BSD
 * sockets. When those are not compiled in the public entry points below fall
 * back to -ENOSYS so we never silently report a fake "up to date" result.
 */
#if defined(CONFIG_HTTP_CLIENT) && defined(CONFIG_NET_SOCKETS)

/* Tunables — fall back to sane defaults if the Kconfig knobs are absent. */
#ifndef CONFIG_AKIRA_OTA_HTTP_TIMEOUT_MS
#define CONFIG_AKIRA_OTA_HTTP_TIMEOUT_MS 30000
#endif
#ifndef CONFIG_AKIRA_OTA_TLS_SEC_TAG
#define CONFIG_AKIRA_OTA_TLS_SEC_TAG 42
#endif
#ifndef CONFIG_AKIRA_OTA_MAX_CHUNK_SIZE
#define CONFIG_AKIRA_OTA_MAX_CHUNK_SIZE 4096
#endif

/* Parsed manifest fields. */
struct ota_manifest {
    char    version[32];             /* e.g. "1.6.3"                  */
    char    fw_url[MANIFEST_URL_MAX]; /* firmware image URL           */
    uint8_t sha256[32];              /* expected image digest         */
    bool    have_sha;
    size_t  size;                    /* image size in bytes, 0=unknown */
};

/* Split an http(s):// URL into host / port / path, honouring an explicit
 * port and preserving any query string. Returns 0 on success. */
static int ota_split_url(const char *url, char *host, size_t host_sz,
                         char *port, size_t port_sz, char *path,
                         size_t path_sz, bool *use_tls)
{
    struct http_parser_url u;
    http_parser_url_init(&u);
    if (http_parser_parse_url(url, strlen(url), 0, &u) != 0) {
        return -EINVAL;
    }
    if (!(u.field_set & (1 << UF_SCHEMA)) || !(u.field_set & (1 << UF_HOST))) {
        return -EINVAL;
    }

    const char *scheme = url + u.field_data[UF_SCHEMA].off;
    uint16_t scheme_len = u.field_data[UF_SCHEMA].len;
    if (scheme_len == 5 && strncmp(scheme, "https", 5) == 0) {
        *use_tls = true;
    } else if (scheme_len == 4 && strncmp(scheme, "http", 4) == 0) {
        *use_tls = false;
    } else {
        return -EPROTO;
    }

    uint16_t hlen = u.field_data[UF_HOST].len;
    if ((size_t)hlen + 1 > host_sz) {
        return -ENOMEM;
    }
    memcpy(host, url + u.field_data[UF_HOST].off, hlen);
    host[hlen] = '\0';

    uint16_t portnum = (u.field_set & (1 << UF_PORT)) ? u.port
                                                      : (*use_tls ? 443 : 80);
    (void)snprintk(port, port_sz, "%u", portnum);

    if (u.field_set & (1 << UF_PATH)) {
        uint16_t off = u.field_data[UF_PATH].off;
        uint16_t len = u.field_data[UF_PATH].len;
        /* Path and query are contiguous in the source buffer, so extend the
         * copy to include "?query" when present. */
        if (u.field_set & (1 << UF_QUERY)) {
            len = (uint16_t)((u.field_data[UF_QUERY].off +
                              u.field_data[UF_QUERY].len) - off);
        }
        if ((size_t)len + 1 > path_sz) {
            return -ENOMEM;
        }
        memcpy(path, url + off, len);
        path[len] = '\0';
    } else {
        if (path_sz < 2) {
            return -ENOMEM;
        }
        path[0] = '/';
        path[1] = '\0';
    }
    return 0;
}

/* Connect an (optionally TLS) TCP socket to host:port. Returns fd or -errno. */
static int ota_connect(const char *host, const char *port, bool use_tls)
{
    struct zsock_addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct zsock_addrinfo *res = NULL;

    int ret = zsock_getaddrinfo(host, port, &hints, &res);
    if (ret != 0 || res == NULL) {
        LOG_ERR("OTA: DNS resolution failed for %s (%d)", host, ret);
        return -EHOSTUNREACH;
    }

    int sock;
    if (use_tls) {
#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
        sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
#else
        LOG_ERR("OTA: https requested but TLS sockets are not enabled");
        zsock_freeaddrinfo(res);
        return -EPROTONOSUPPORT;
#endif
    } else {
        sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TCP);
    }
    if (sock < 0) {
        LOG_ERR("OTA: socket() failed (errno %d)", errno);
        zsock_freeaddrinfo(res);
        return -ENOMEM;
    }

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
    if (use_tls) {
        static const sec_tag_t sec_tags[] = { CONFIG_AKIRA_OTA_TLS_SEC_TAG };
        if (zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
                             sec_tags, sizeof(sec_tags)) < 0 ||
            zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
                             host, strlen(host) + 1) < 0) {
            LOG_ERR("OTA: TLS configuration failed (errno %d)", errno);
            zsock_close(sock);
            zsock_freeaddrinfo(res);
            return -ECONNREFUSED;
        }
    }
#endif

    ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);
    if (ret < 0) {
        LOG_ERR("OTA: connect to %s:%s failed (errno %d)", host, port, errno);
        zsock_close(sock);
        return -ECONNREFUSED;
    }
    return sock;
}

/* Minimal flat-JSON helpers: the manifest is a small object emitted by our
 * build server, so a full parser is unnecessary. */
static int ota_json_get_string(const char *json, const char *key,
                               char *out, size_t out_sz)
{
    char pat[40];
    int n = snprintk(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || n >= (int)sizeof(pat)) {
        return -EINVAL;
    }
    const char *p = strstr(json, pat);
    if (!p) {
        return -ENOENT;
    }
    p += n;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return -EINVAL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != '"') return -EINVAL;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && *(p + 1)) p++;   /* skip escape sequence */
        out[i++] = *p++;
    }
    if (*p != '"') return -EINVAL;
    out[i] = '\0';
    return 0;
}

static int ota_json_get_uint(const char *json, const char *key,
                             unsigned long *out)
{
    char pat[40];
    int n = snprintk(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || n >= (int)sizeof(pat)) {
        return -EINVAL;
    }
    const char *p = strstr(json, pat);
    if (!p) return -ENOENT;
    p += n;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return -EINVAL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p < '0' || *p > '9') return -EINVAL;
    unsigned long v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10u + (unsigned)(*p - '0'); p++; }
    *out = v;
    return 0;
}

static int ota_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int ota_hex_to_sha256(const char *hex, uint8_t out[32])
{
    if (strlen(hex) < 64) {
        return -EINVAL;
    }
    for (int i = 0; i < 32; i++) {
        int hi = ota_hex_nibble(hex[2 * i]);
        int lo = ota_hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -EINVAL;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int ota_parse_manifest(const char *json, struct ota_manifest *out)
{
    memset(out, 0, sizeof(*out));

    if (ota_json_get_string(json, "version", out->version,
                            sizeof(out->version)) < 0) {
        LOG_ERR("OTA: manifest is missing 'version'");
        return -EINVAL;
    }

    /* Firmware URL: accept the common key spellings our tooling may emit. */
    if (ota_json_get_string(json, "url", out->fw_url,
                            sizeof(out->fw_url)) < 0 &&
        ota_json_get_string(json, "firmware_url", out->fw_url,
                            sizeof(out->fw_url)) < 0 &&
        ota_json_get_string(json, "download_url", out->fw_url,
                            sizeof(out->fw_url)) < 0 &&
        ota_json_get_string(json, "firmware", out->fw_url,
                            sizeof(out->fw_url)) < 0) {
        out->fw_url[0] = '\0';   /* only fatal for fetch_and_apply */
    }

    char sha_hex[80];
    if (ota_json_get_string(json, "sha256", sha_hex, sizeof(sha_hex)) == 0 ||
        ota_json_get_string(json, "hash", sha_hex, sizeof(sha_hex)) == 0) {
        if (ota_hex_to_sha256(sha_hex, out->sha256) == 0) {
            out->have_sha = true;
        }
    }

    unsigned long sz = 0;
    if (ota_json_get_uint(json, "size", &sz) == 0) {
        out->size = (size_t)sz;
    }
    return 0;
}

/* Accumulate the (small) manifest body into a caller-provided buffer. */
struct ota_manifest_rx {
    char  *buf;
    size_t cap;
    size_t len;
    bool   overflow;
};

static int ota_manifest_cb(struct http_response *rsp,
                           enum http_final_call final_data, void *user_data)
{
    struct ota_manifest_rx *rx = user_data;

    if (rsp->body_frag_start && rsp->body_frag_len) {
        size_t space = (rx->len + 1 < rx->cap) ? (rx->cap - 1 - rx->len) : 0;
        size_t nbytes = rsp->body_frag_len;
        if (nbytes > space) {
            nbytes = space;
            rx->overflow = true;
        }
        if (nbytes) {
            memcpy(rx->buf + rx->len, rsp->body_frag_start, nbytes);
            rx->len += nbytes;
        }
    }
    if (final_data == HTTP_DATA_FINAL) {
        rx->buf[rx->len] = '\0';
    }
    return 0;
}

/* Fetch and parse the manifest at @manifest_url. Returns 0 on success. */
static int ota_fetch_manifest(const char *manifest_url,
                              struct ota_manifest *out)
{
    char host[128];
    char port[8];
    char path[MANIFEST_URL_MAX];
    bool use_tls;

    int ret = ota_split_url(manifest_url, host, sizeof(host), port,
                            sizeof(port), path, sizeof(path), &use_tls);
    if (ret < 0) {
        LOG_ERR("OTA: cannot parse manifest URL (%d)", ret);
        return ret;
    }

    int sock = ota_connect(host, port, use_tls);
    if (sock < 0) {
        return sock;
    }

    static uint8_t recv_buf[1024];
    char body[1024];
    struct ota_manifest_rx rx = {
        .buf = body, .cap = sizeof(body), .len = 0, .overflow = false,
    };

    static const char *const headers[] = {
        "Connection: close\r\n",
        "Accept: application/json\r\n",
        NULL,
    };
    struct http_request req = { 0 };
    req.method = HTTP_GET;
    req.url = path;
    req.host = host;
    req.port = port;
    req.protocol = "HTTP/1.1";
    req.response = ota_manifest_cb;
    req.recv_buf = recv_buf;
    req.recv_buf_len = sizeof(recv_buf);
    req.header_fields = headers;

    ret = http_client_req(sock, &req, CONFIG_AKIRA_OTA_HTTP_TIMEOUT_MS, &rx);
    uint16_t status = req.internal.response.http_status_code;
    zsock_close(sock);

    if (ret < 0) {
        LOG_ERR("OTA: manifest request failed (%d)", ret);
        return ret;
    }
    if (status != 200) {
        LOG_ERR("OTA: manifest HTTP status %u", status);
        return -EIO;
    }
    if (rx.overflow) {
        LOG_ERR("OTA: manifest larger than %zu bytes", sizeof(body));
        return -EMSGSIZE;
    }
    return ota_parse_manifest(body, out);
}

/* Parse a dotted version string into a triple (ignores pre-release/build
 * metadata after the third field). Returns 0 on success. */
static int ota_version_parse(const char *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (!s || !*s) {
        return -EINVAL;
    }
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    int idx = 0;
    bool got_digit = false;
    while (*s && idx < 3) {
        if (*s >= '0' && *s <= '9') {
            v[idx] = v[idx] * 10 + (*s - '0');
            got_digit = true;
            s++;
        } else if (*s == '.') {
            idx++;
            s++;
        } else {
            break;
        }
    }
    return got_digit ? 0 : -EINVAL;
}

/* Returns >0 if a is newer than b, <0 if older, 0 if equal. */
static int ota_version_cmp(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; i++) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

#endif /* CONFIG_HTTP_CLIENT && CONFIG_NET_SOCKETS */

#if defined(CONFIG_HTTP_CLIENT) && defined(CONFIG_NET_SOCKETS) && \
    defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)

/* Streams the firmware body straight into the OTA manager, chunk by chunk. */
struct ota_fw_rx {
    size_t  written;
    size_t  total;
    int     err;        /* first negative errno from a write, else 0 */
    uint8_t last_pct;
};

static int ota_firmware_cb(struct http_response *rsp,
                           enum http_final_call final_data, void *user_data)
{
    struct ota_fw_rx *rx = user_data;

    if (rx->err) {
        return rx->err;   /* abort the download */
    }
    if (rx->total == 0 && rsp->content_length) {
        rx->total = rsp->content_length;
    }

    if (rsp->body_frag_start && rsp->body_frag_len) {
        enum ota_result r = ota_write_chunk(rsp->body_frag_start,
                                            rsp->body_frag_len);
        if (r != OTA_OK) {
            LOG_ERR("OTA: write_chunk failed (%d)", r);
            rx->err = -EIO;
            return rx->err;
        }
        rx->written += rsp->body_frag_len;

        if (rx->total) {
            uint8_t pct = (uint8_t)(((uint64_t)rx->written * 100u) / rx->total);
            if (pct != rx->last_pct) {
                rx->last_pct = pct;
#ifdef CONFIG_AKIRA_TELEMETRY
                akira_telemetry_ota_progress((uint32_t)rx->written,
                                             (uint32_t)rx->total);
#endif
            }
        }
    }
    ARG_UNUSED(final_data);
    return 0;
}

/* Download the firmware described by @m and stage it in the secondary slot. */
static int ota_download_and_apply(const struct ota_manifest *m)
{
    char host[128];
    char port[8];
    char path[MANIFEST_URL_MAX];
    bool use_tls;

    int ret = ota_split_url(m->fw_url, host, sizeof(host), port, sizeof(port),
                            path, sizeof(path), &use_tls);
    if (ret < 0) {
        LOG_ERR("OTA: cannot parse firmware URL (%d)", ret);
        return ret;
    }

    int sock = ota_connect(host, port, use_tls);
    if (sock < 0) {
        return sock;
    }

    /* Prepare the secondary slot (erases it). */
    enum ota_result r = ota_start_update(m->size);
    if (r != OTA_OK) {
        LOG_ERR("OTA: start_update failed (%d)", r);
        zsock_close(sock);
        return -EIO;
    }

    if (m->have_sha) {
        r = ota_set_expected_sha256(m->sha256);
        if (r != OTA_OK) {
            LOG_ERR("OTA: set_expected_sha256 failed (%d)", r);
            ota_abort_update();
            zsock_close(sock);
            return -EIO;
        }
    } else if (IS_ENABLED(CONFIG_AKIRA_OTA_REQUIRE_HASH)) {
        LOG_ERR("OTA: manifest has no sha256 but AKIRA_OTA_REQUIRE_HASH=y");
        ota_abort_update();
        zsock_close(sock);
        return -EPERM;
    }

    static uint8_t recv_buf[CONFIG_AKIRA_OTA_MAX_CHUNK_SIZE];
    struct ota_fw_rx rx = { .total = m->size };

    static const char *const headers[] = {
        "Connection: close\r\n",
        NULL,
    };
    struct http_request req = { 0 };
    req.method = HTTP_GET;
    req.url = path;
    req.host = host;
    req.port = port;
    req.protocol = "HTTP/1.1";
    req.response = ota_firmware_cb;
    req.recv_buf = recv_buf;
    req.recv_buf_len = sizeof(recv_buf);
    req.header_fields = headers;

#ifdef CONFIG_AKIRA_TELEMETRY
    akira_telemetry_ota_progress(0, (uint32_t)m->size);
#endif

    ret = http_client_req(sock, &req, CONFIG_AKIRA_OTA_HTTP_TIMEOUT_MS, &rx);
    uint16_t status = req.internal.response.http_status_code;
    zsock_close(sock);

    if (rx.err) {
        ota_abort_update();
        return rx.err;
    }
    if (ret < 0) {
        LOG_ERR("OTA: firmware request failed (%d)", ret);
        ota_abort_update();
        return ret;
    }
    if (status != 200) {
        LOG_ERR("OTA: firmware HTTP status %u", status);
        ota_abort_update();
        return -EIO;
    }
    if (rx.written == 0) {
        LOG_ERR("OTA: empty firmware body");
        ota_abort_update();
        return -EIO;
    }

    /* Recomputes and verifies the SHA-256 against ota_set_expected_sha256(). */
    r = ota_finalize_update();
    if (r != OTA_OK) {
        LOG_ERR("OTA: finalize failed (%d) - hash mismatch or invalid image", r);
        ota_abort_update();
        return -EIO;
    }

#ifdef CONFIG_AKIRA_TELEMETRY
    akira_telemetry_ota_progress((uint32_t)rx.written, (uint32_t)rx.written);
#endif
    LOG_INF("OTA: image staged (%zu bytes); reboot to apply", rx.written);
    return 0;
}

#endif /* net + flash */

/* ── ota_check ──────────────────────────────────────────────────────────── */

int akira_native_ota_check(wasm_exec_env_t exec_env, const char *manifest_url)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_OTA_TRIGGER, -EPERM);

    if (!manifest_url) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_native_addr(inst, (void *)manifest_url,
                                           strnlen(manifest_url,
                                                   MANIFEST_URL_MAX) + 1)) {
        return -EFAULT;
    }

    int ret = validate_https_url(manifest_url);
    if (ret < 0) {
        return ret;
    }

#if defined(CONFIG_HTTP_CLIENT) && defined(CONFIG_NET_SOCKETS)
    struct ota_manifest m;
    ret = ota_fetch_manifest(manifest_url, &m);
    if (ret < 0) {
        LOG_WRN("OTA check: manifest fetch failed (%d)", ret);
        return ret;
    }

    int mv[3];
    if (ota_version_parse(m.version, mv) < 0) {
        LOG_ERR("OTA check: invalid manifest version '%s'", m.version);
        return -EINVAL;
    }

    const int cur[3] = {
        AKIRA_VERSION_MAJOR, AKIRA_VERSION_MINOR, AKIRA_VERSION_PATCH,
    };

    /* Anti-rollback: only a strictly newer version counts as an update.
     * An equal or older manifest reports "up to date" (0), never 1. */
    if (ota_version_cmp(mv, cur) > 0) {
        LOG_INF("OTA check: update available %d.%d.%d -> %s",
                cur[0], cur[1], cur[2], m.version);
        return 1;
    }

    LOG_INF("OTA check: up to date (current %d.%d.%d, manifest %s)",
            cur[0], cur[1], cur[2], m.version);
    return 0;
#else
    LOG_WRN("OTA check: HTTP client not enabled (CONFIG_HTTP_CLIENT)");
    return -ENOSYS;
#endif
}

/* ── ota_fetch_and_apply ────────────────────────────────────────────────── */

int akira_native_ota_fetch_and_apply(wasm_exec_env_t exec_env,
                                      const char *manifest_url)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_OTA_TRIGGER, -EPERM);

    if (!manifest_url) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_native_addr(inst, (void *)manifest_url,
                                           strnlen(manifest_url,
                                                   MANIFEST_URL_MAX) + 1)) {
        return -EFAULT;
    }

    int ret = validate_https_url(manifest_url);
    if (ret < 0) {
        return ret;
    }

    /* Check OTA is not already in progress */
#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    const struct ota_progress *p = ota_get_progress();
    if (p && p->state == OTA_STATE_IN_PROGRESS) {
        return -EBUSY;
    }
#endif

#if defined(CONFIG_HTTP_CLIENT) && defined(CONFIG_NET_SOCKETS) && \
    defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    /* 1. Fetch + parse the manifest. */
    struct ota_manifest m;
    ret = ota_fetch_manifest(manifest_url, &m);
    if (ret < 0) {
        LOG_ERR("OTA apply: manifest fetch failed (%d)", ret);
        return ret;
    }

    int mv[3];
    if (ota_version_parse(m.version, mv) < 0) {
        LOG_ERR("OTA apply: invalid manifest version '%s'", m.version);
        return -EINVAL;
    }

    const int cur[3] = {
        AKIRA_VERSION_MAJOR, AKIRA_VERSION_MINOR, AKIRA_VERSION_PATCH,
    };

    /* 2. Anti-rollback (H3): refuse to install a version that is not strictly
     *    newer than the running firmware. Blocks downgrade / replay of an old
     *    (possibly still validly signed) image. */
    if (ota_version_cmp(mv, cur) <= 0) {
        LOG_WRN("OTA apply: refusing non-newer version %s (current %d.%d.%d)",
                m.version, cur[0], cur[1], cur[2]);
        return -EACCES;
    }

    /* 3. Validate the firmware URL. */
    if (m.fw_url[0] == '\0') {
        LOG_ERR("OTA apply: manifest has no firmware URL");
        return -EINVAL;
    }
#ifndef CONFIG_AKIRA_OTA_ALLOW_INSECURE_HTTP
    if (validate_https_url(m.fw_url) < 0) {
        LOG_ERR("OTA apply: firmware URL must be https");
        return -EPROTO;
    }
#endif

    /* 4. Download, verify (SHA-256) and stage the image in the 2nd slot. */
    ret = ota_download_and_apply(&m);
    if (ret < 0) {
        LOG_ERR("OTA apply: download/stage failed (%d)", ret);
        return ret;
    }

    LOG_INF("OTA apply: firmware %s staged; reboot to apply", m.version);
    return 0;
#else
    LOG_WRN("OTA apply: requires CONFIG_HTTP_CLIENT + MCUboot flash map");
    return -ENOSYS;
#endif
}

/* ── ota_get_state ──────────────────────────────────────────────────────── */

int akira_native_ota_get_state(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_OTA_TRIGGER, -EPERM);

#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    const struct ota_progress *p = ota_get_progress();
    if (!p) {
        return -ENODEV;
    }
    return (int)p->state;
#else
    return -ENOTSUP;
#endif
}

/* ── ota_confirm ────────────────────────────────────────────────────────── */

int akira_native_ota_confirm(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_OTA_TRIGGER, -EPERM);

#ifdef CONFIG_AKIRA_BOOT_GUARD
    return akira_boot_guard_confirm();
#elif defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    enum ota_result r = ota_confirm_firmware();
    return (r == OTA_OK) ? 0 : -EIO;
#else
    return -ENOTSUP;
#endif
}

/* ── ota_rollback ───────────────────────────────────────────────────────── */

int akira_native_ota_rollback(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_OTA_TRIGGER, -EPERM);

#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    enum ota_result r = ota_request_rollback();
    return (r == OTA_OK) ? 0 : -EIO;
#else
    return -ENOTSUP;
#endif
}

#endif /* CONFIG_AKIRA_WASM_OTA */
