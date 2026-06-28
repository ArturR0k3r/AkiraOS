/**
 * @file http_routes.c
 * @brief AkiraOS HTTP API Route Handlers
 *
 * Implements and registers all REST API routes. Migrates the two endpoints
 * previously served by the deleted web_server.c (/upload, /api/apps/install)
 * and adds the new /api/v1/... management API.
 */

#define _GNU_SOURCE
#include "http_routes.h"
#include "http_server.h"
#include "buf_pool.h"
#include "transport_interface.h"
#include "akira.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "drivers/platform_hal.h"

#ifdef CONFIG_AKIRA_APP_MANAGER
#include "runtime/app_manager/app_manager.h"
#endif

#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
#include "ota/ota_manager.h"
#define AKIRA_HTTP_OTA 1
#endif

#ifdef CONFIG_AKIRA_POWER_MANAGER
#include "drivers/power/power_manager.h"
#endif

#ifdef CONFIG_POWEROFF
#include <zephyr/sys/poweroff.h>
#endif

#ifdef CONFIG_AKIRA_SETTINGS
#include "settings/settings.h"
#endif

LOG_MODULE_REGISTER(http_routes, CONFIG_AKIRA_LOG_LEVEL);

/* TCP_NODELAY may not be defined on all platforms */
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

/*===========================================================================*/
/* Auth helper                                                               */
/*===========================================================================*/

static bool route_check_auth(const http_request_t *req)
{
#ifdef CONFIG_AKIRA_HTTP_NO_AUTH
    return true;
#else
#ifdef CONFIG_AKIRA_HTTP_UPLOAD_TOKEN
    /* sizeof() on a string literal gives length + 1; <= 1 means empty string */
    if (sizeof(CONFIG_AKIRA_HTTP_UPLOAD_TOKEN) <= 1)
    {
        return true;
    }
    if (!req->raw)
    {
        return false;
    }
    const char *auth = strstr(req->raw, "Authorization: Bearer ");
    if (!auth)
    {
        return false;
    }
    auth += 22; /* skip "Authorization: Bearer " */

    const char *end = strpbrk(auth, "\r\n");
    size_t given_len = end ? (size_t)(end - auth) : strlen(auth);
    size_t expected_len = sizeof(CONFIG_AKIRA_HTTP_UPLOAD_TOKEN) - 1;

    if (given_len != expected_len)
    {
        return false;
    }
    /* Constant-time comparison */
    uint8_t diff = 0;
    for (size_t i = 0; i < expected_len; i++)
    {
        diff |= (uint8_t)auth[i] ^ (uint8_t)CONFIG_AKIRA_HTTP_UPLOAD_TOKEN[i];
    }
    return diff == 0;
#else
    return true;
#endif
#endif /* CONFIG_AKIRA_HTTP_NO_AUTH */
}

/*===========================================================================*/
/* Query-param helper                                                        */
/*===========================================================================*/

static bool query_param(const char *query, const char *key,
                        char *out, size_t out_len)
{
    if (!query || !key || !out || out_len == 0)
    {
        return false;
    }
    size_t key_len = strlen(key);
    const char *p = query;
    while (p && *p)
    {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=')
        {
            const char *val = p + key_len + 1;
            const char *end = strchr(val, '&');
            size_t len = end ? (size_t)(end - val) : strlen(val);
            if (len >= out_len)
            {
                len = out_len - 1;
            }
            memcpy(out, val, len);
            out[len] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p)
        {
            p++;
        }
    }
    return false;
}

/*===========================================================================*/
/* Static response: set by last upload chunk, read by http_server.c         */
/*===========================================================================*/

static char upload_resp_buf[128] = "{\"status\":\"ok\"}";
/* Single scratch buffer shared by all route handlers (HTTP is single-threaded). */
static char s_route_buf[1024];

static void __attribute__((unused)) set_upload_resp(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(upload_resp_buf, sizeof(upload_resp_buf), fmt, ap);
    va_end(ap);
    akira_http_set_upload_response(upload_resp_buf);
}

/*===========================================================================*/
/* GET /api/v1/info                                                          */
/*===========================================================================*/

static int route_info(const http_request_t *req, http_response_t *res,
                      void *user_data)
{
    snprintf(s_route_buf, sizeof(s_route_buf),
             "{\"name\":\"%s\","
             "\"fw_version\":\"" AKIRA_VERSION_STRING "\","
             "\"chip\":\"%s\"}",
             CONFIG_AKIRA_DEVICE_NAME,
             akira_get_platform_name());

    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
}

/*===========================================================================*/
/* GET /api/v1/status                                                        */
/*===========================================================================*/

static int route_status(const http_request_t *req, http_response_t *res,
                        void *user_data)
{
    uint32_t storage_free = 0;
    uint32_t storage_total = 0;

#ifdef CONFIG_FILE_SYSTEM
    struct fs_statvfs stat;
    if (fs_statvfs("/lfs", &stat) == 0)
    {
        storage_free = (uint32_t)((uint64_t)stat.f_bfree * stat.f_frsize);
        storage_total = (uint32_t)((uint64_t)stat.f_blocks * stat.f_frsize);
    }
#endif

    snprintf(s_route_buf, sizeof(s_route_buf),
             "{\"storage_free\":%u,\"storage_total\":%u,\"uptime_s\":%u}",
             storage_free, storage_total,
             (uint32_t)(k_uptime_get() / 1000U));

    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
}

/*===========================================================================*/
/* GET /api/v1/apps                                                          */
/*===========================================================================*/

static int route_apps_list(const http_request_t *req, http_response_t *res,
                           void *user_data)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    app_info_t apps[16];
    int count = app_manager_list(apps, ARRAY_SIZE(apps));
    if (count < 0)
    {
        count = 0;
    }

    int pos = 0;
    pos += snprintf(s_route_buf + pos, sizeof(s_route_buf) - pos, "[");
    for (int i = 0; i < count && pos < (int)sizeof(s_route_buf) - 4; i++)
    {
        const char *state_str;
        switch (apps[i].state)
        {
        case APP_STATE_INSTALLED:
            state_str = "installed";
            break;
        case APP_STATE_RUNNING:
            state_str = "running";
            break;
        case APP_STATE_STOPPED:
            state_str = "stopped";
            break;
        case APP_STATE_ERROR:
            state_str = "error";
            break;
        case APP_STATE_FAILED:
            state_str = "failed";
            break;
        default:
            state_str = "new";
            break;
        }
        pos += snprintf(s_route_buf + pos, sizeof(s_route_buf) - pos,
                        "%s{\"id\":%d,\"name\":\"%s\",\"version\":\"%s\","
                        "\"state\":\"%s\",\"size\":%u,\"crashes\":%u}",
                        i > 0 ? "," : "",
                        apps[i].id, apps[i].name, apps[i].version,
                        state_str, apps[i].size, apps[i].crash_count);
    }
    snprintf(s_route_buf + pos, sizeof(s_route_buf) - pos, "]");
#else
    strncpy(s_route_buf, "[]", sizeof(s_route_buf));
#endif

    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
}

/*===========================================================================*/
/* GET /api/v1/logs                                                          */
/*===========================================================================*/

static int route_logs(const http_request_t *req, http_response_t *res,
                      void *user_data)
{
    /* TODO: wire up a Zephyr log backend ring buffer */
    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = "{\"logs\":[]}";
    return 0;
}

/*===========================================================================*/
/* POST /api/v1/apps/start                                                   */
/*===========================================================================*/

static int route_app_start(const http_request_t *req, http_response_t *res,
                           void *user_data)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    char name[APP_NAME_MAX_LEN] = {0};
    if (!query_param(req->query, "name", name, sizeof(name)) || name[0] == '\0')
    {
        res->status_code = 400;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"missing ?name\"}";
        return 0;
    }
    int ret = app_manager_start(name);
    if (ret < 0)
    {
        snprintf(s_route_buf, sizeof(s_route_buf), "{\"error\":\"start failed: %d\"}", ret);
        res->status_code = 500;
    }
    else
    {
        snprintf(s_route_buf, sizeof(s_route_buf), "{\"status\":\"started\",\"name\":\"%s\"}", name);
        res->status_code = 200;
    }
#else
    strncpy(s_route_buf, "{\"error\":\"app manager disabled\"}", sizeof(s_route_buf));
    res->status_code = 501;
#endif

    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
}

/*===========================================================================*/
/* POST /api/v1/apps/stop                                                    */
/*===========================================================================*/

static int route_app_stop(const http_request_t *req, http_response_t *res,
                          void *user_data)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    char name[APP_NAME_MAX_LEN] = {0};
    if (!query_param(req->query, "name", name, sizeof(name)) || name[0] == '\0')
    {
        res->status_code = 400;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"missing ?name\"}";
        return 0;
    }
    int ret = app_manager_stop(name);
    if (ret < 0)
    {
        snprintf(s_route_buf, sizeof(s_route_buf), "{\"error\":\"stop failed: %d\"}", ret);
        res->status_code = 500;
    }
    else
    {
        snprintf(s_route_buf, sizeof(s_route_buf), "{\"status\":\"stopped\",\"name\":\"%s\"}", name);
        res->status_code = 200;
    }
#else
    strncpy(s_route_buf, "{\"error\":\"app manager disabled\"}", sizeof(s_route_buf));
    res->status_code = 501;
#endif

    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
}

/*===========================================================================*/
/* DELETE /api/v1/apps                                                       */
/*===========================================================================*/

static int route_app_delete(const http_request_t *req, http_response_t *res,
                            void *user_data)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    char name[APP_NAME_MAX_LEN] = {0};
    if (!query_param(req->query, "name", name, sizeof(name)) || name[0] == '\0')
    {
        res->status_code = 400;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"missing ?name\"}";
        return 0;
    }
    int ret = app_manager_uninstall(name);
    if (ret < 0)
    {
        snprintf(s_route_buf, sizeof(s_route_buf), "{\"error\":\"uninstall failed: %d\"}", ret);
        res->status_code = 500;
    }
    else
    {
        snprintf(s_route_buf, sizeof(s_route_buf), "{\"status\":\"deleted\",\"name\":\"%s\"}", name);
        res->status_code = 200;
    }
#else
    strncpy(s_route_buf, "{\"error\":\"app manager disabled\"}", sizeof(s_route_buf));
    res->status_code = 501;
#endif

    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
}

/*===========================================================================*/
/* POST /api/apps/install — streaming WASM app upload                        */
/*                                                                           */
/* Uses req->client_fd for direct streaming recv, exactly like the old      */
/* web_server.c did.  The HTTP server framework passes client_fd through    */
/* http_request_t so handlers can stream large bodies.                      */
/*===========================================================================*/

static int route_app_install(const http_request_t *req, http_response_t *res,
                             void *user_data)
{
#ifndef CONFIG_AKIRA_APP_MANAGER
    res->status_code = 501;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = "{\"error\":\"app manager disabled\"}";
    return 0;
#else

    if (!route_check_auth(req))
    {
        res->status_code = 401;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"Unauthorized\"}";
        return 0;
    }

    size_t content_length = req->content_length;
    if (content_length == 0 ||
        content_length > (size_t)CONFIG_AKIRA_APP_MAX_SIZE_KB * 1024)
    {
        res->status_code = 400;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"Invalid Content-Length\"}";
        return 0;
    }

    /* Extract app name from query param */
    char app_name[APP_NAME_MAX_LEN] = "uploaded_app";
    query_param(req->query, "name", app_name, sizeof(app_name));

    /* Begin chunked install session */
    int session = app_manager_install_begin(app_name, content_length,
                                            APP_SOURCE_HTTP);
    if (session < 0)
    {
        snprintf(s_route_buf, sizeof(s_route_buf),
                 "{\"error\":\"install_begin failed: %d\"}", session);
        res->status_code = 500;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = s_route_buf;
        return 0;
    }

    /* Write initial body chunk already read into the HTTP buffer */
    size_t total_received = 0;
    if (req->body && req->body_len > 0)
    {
        int ret = app_manager_install_chunk(session,
                                            req->body, req->body_len);
        if (ret < 0)
        {
            app_manager_install_abort(session);
            snprintf(s_route_buf, sizeof(s_route_buf),
                     "{\"error\":\"chunk write failed: %d\"}", ret);
            res->status_code = 500;
            res->content_type = HTTP_CONTENT_JSON;
            res->body = s_route_buf;
            return 0;
        }
        total_received = req->body_len;
    }

    /* Stream the remaining body directly from the socket */
    struct akira_buf *buf = akira_buf_alloc(K_MSEC(200));
    if (!buf)
    {
        app_manager_install_abort(session);
        res->status_code = 503;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"Server busy\"}";
        return 0;
    }

    /* Set a 60 s receive timeout for large uploads */
    struct timeval tv = {.tv_sec = 60, .tv_usec = 0};
    setsockopt(req->client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (total_received < content_length)
    {
        akira_buf_reset(buf);
        size_t want = MIN(AKIRA_BUF_SIZE,
                          content_length - total_received);
        ssize_t got = recv(req->client_fd, buf->data, want, 0);
        if (got <= 0)
        {
            LOG_ERR("Upload recv failed at %zu/%zu: errno %d",
                    total_received, content_length, errno);
            akira_buf_unref(buf);
            app_manager_install_abort(session);
            res->status_code = 500;
            res->content_type = HTTP_CONTENT_JSON;
            res->body = "{\"error\":\"Upload incomplete\"}";
            return 0;
        }
        akira_buf_add_len(buf, got);

        int ret = app_manager_install_chunk(session,
                                            (const char *)buf->data, buf->len);
        if (ret < 0)
        {
            akira_buf_unref(buf);
            app_manager_install_abort(session);
            snprintf(s_route_buf, sizeof(s_route_buf),
                     "{\"error\":\"chunk write failed: %d\"}", ret);
            res->status_code = 500;
            res->content_type = HTTP_CONTENT_JSON;
            res->body = s_route_buf;
            return 0;
        }
        total_received += got;
        k_yield();
    }
    akira_buf_unref(buf);

    /* Finalise */
    int app_id = app_manager_install_end(session, NULL);
    if (app_id < 0)
    {
        snprintf(s_route_buf, sizeof(s_route_buf),
                 "{\"error\":\"install_end failed: %d\"}", app_id);
        res->status_code = 500;
    }
    else
    {
        snprintf(s_route_buf, sizeof(s_route_buf),
                 "{\"status\":\"installed\",\"name\":\"%s\",\"id\":%d}",
                 app_name, app_id);
        res->status_code = 200;
    }

    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
#endif /* CONFIG_AKIRA_APP_MANAGER */
}

/*===========================================================================*/
/* POST /upload — OTA firmware flash (streaming)                             */
/*                                                                           */
/* Migrated from the deleted web_server.c.  Uses the transport_* interface  */
/* and direct socket recv for zero-copy streaming.                          */
/*===========================================================================*/

static int route_firmware_upload(const http_request_t *req,
                                 http_response_t *res, void *user_data)
{
#if !defined(CONFIG_FLASH_MAP) || !defined(CONFIG_BOOTLOADER_MCUBOOT)
    res->status_code = 501;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = "{\"error\":\"OTA not supported on this build\"}";
    return 0;
#else
    if (!route_check_auth(req))
    {
        res->status_code = 401;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"Unauthorized\"}";
        return 0;
    }

    size_t content_length = req->content_length;
    if (content_length == 0 || content_length > (2U * 1024U * 1024U))
    {
        res->status_code = 400;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"Invalid file size\"}";
        return 0;
    }

    /* Locate multipart boundary in the raw headers */
    if (!req->raw)
    {
        res->status_code = 400;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"Invalid request\"}";
        return 0;
    }

    /* Extract boundary from Content-Type header */
    char boundary[128] = {0};
    const char *ct = strstr(req->raw, "Content-Type:");
    const char *bp = ct ? strstr(ct, "boundary=") : NULL;
    if (!bp)
    {
        res->status_code = 400;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"No multipart boundary\"}";
        return 0;
    }
    bp += 9;
    const char *bp_end = bp;
    while (*bp_end && *bp_end != ' ' && *bp_end != '\r' &&
           *bp_end != '\n' && *bp_end != ';')
    {
        bp_end++;
    }
    size_t blen = MIN((size_t)(bp_end - bp), sizeof(boundary) - 1);
    memcpy(boundary, bp, blen);

    LOG_INF("FW upload: len=%zu boundary=%s", content_length, boundary);

    /* Signal transport layer: begin firmware transfer */
    int ret = transport_begin(TRANSPORT_DATA_FIRMWARE, content_length,
                              "firmware.bin");
    if (ret != 0)
    {
        LOG_ERR("transport_begin failed: %d", ret);
        res->status_code = 500;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"Failed to start OTA\"}";
        return 0;
    }

    /* Set 60 s receive timeout */
    struct timeval tv = {.tv_sec = 60, .tv_usec = 0};
    setsockopt(req->client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t total_written = 0;
    size_t total_received = 0;
    bool past_part_header = false;
    size_t boundary_len = strlen(boundary);
    uint8_t last_progress = 0;

    struct transport_chunk_info cinfo = {
        .type = TRANSPORT_DATA_FIRMWARE,
        .total_size = content_length,
        .offset = 0,
        .flags = 0,
        .name = "firmware.bin",
    };

    struct akira_buf *ubuf = akira_buf_alloc(K_MSEC(200));
    if (!ubuf)
    {
        transport_abort(TRANSPORT_DATA_FIRMWARE);
        res->status_code = 503;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"Server busy\"}";
        return 0;
    }

    /* Process initial body data already in the HTTP buffer */
    if (req->body && req->body_len > 0)
    {
        const char *data_start = req->body;
        size_t data_len = req->body_len;

        if (!past_part_header)
        {
            const char *hdr_end = strstr(data_start, "\r\n\r\n");
            if (hdr_end)
            {
                hdr_end += 4;
                data_len -= (hdr_end - data_start);
                data_start = hdr_end;
                past_part_header = true;
            }
        }

        if (past_part_header && data_len > 0)
        {
            cinfo.offset = total_written;
            ret = transport_notify(TRANSPORT_DATA_FIRMWARE,
                                   (const uint8_t *)data_start, data_len,
                                   &cinfo);
            if (ret != 0)
            {
                akira_buf_unref(ubuf);
                transport_abort(TRANSPORT_DATA_FIRMWARE);
                res->status_code = 500;
                res->content_type = HTTP_CONTENT_JSON;
                res->body = "{\"error\":\"OTA write failed\"}";
                return 0;
            }
            total_written += data_len;
        }
        total_received = req->body_len;
    }

    /* Stream remaining data */
    while (total_received < content_length)
    {
        akira_buf_reset(ubuf);
        size_t want = MIN(AKIRA_BUF_SIZE,
                          content_length - total_received);
        ssize_t got = recv(req->client_fd, ubuf->data, want, 0);
        if (got <= 0)
        {
            LOG_ERR("FW upload recv failed at %zu/%zu",
                    total_received, content_length);
            akira_buf_unref(ubuf);
            transport_abort(TRANSPORT_DATA_FIRMWARE);
            res->status_code = 500;
            res->content_type = HTTP_CONTENT_JSON;
            res->body = "{\"error\":\"Upload failed\"}";
            return 0;
        }
        akira_buf_add_len(ubuf, got);
        total_received += got;

        const uint8_t *chunk_data = ubuf->data;
        size_t chunk_len = ubuf->len;

        /* Strip multipart part header on first recv if needed */
        if (!past_part_header)
        {
            const char *hdr_end = strstr((const char *)ubuf->data, "\r\n\r\n");
            if (hdr_end)
            {
                hdr_end += 4;
                chunk_len -= (size_t)(hdr_end - (const char *)ubuf->data);
                chunk_data = (const uint8_t *)hdr_end;
                past_part_header = true;
            }
            else
            {
                k_yield();
                continue; /* still buffering multipart header */
            }
        }

        /* Detect closing boundary at end of data */
        uint8_t *end_bound = memmem(chunk_data, chunk_len,
                                    boundary, boundary_len);
        if (end_bound)
        {
            chunk_len = (size_t)(end_bound - chunk_data);
            if (chunk_len >= 2)
            {
                chunk_len -= 2; /* strip preceding \r\n */
            }
        }

        if (chunk_len > 0)
        {
            cinfo.offset = total_written;
            ret = transport_notify(TRANSPORT_DATA_FIRMWARE,
                                   chunk_data, chunk_len, &cinfo);
            if (ret != 0)
            {
                akira_buf_unref(ubuf);
                transport_abort(TRANSPORT_DATA_FIRMWARE);
                res->status_code = 500;
                res->content_type = HTTP_CONTENT_JSON;
                res->body = "{\"error\":\"OTA write failed\"}";
                return 0;
            }
            total_written += chunk_len;
        }

        uint8_t progress = (uint8_t)((total_written * 100U) / content_length);
        if (progress >= last_progress + 10U)
        {
            LOG_INF("OTA: %u%% (%zu B)", progress, total_written);
            last_progress = progress;
        }

        if (end_bound)
        {
            break; /* closing boundary found */
        }
        k_yield();
    }

    akira_buf_unref(ubuf);

    if (total_written == 0)
    {
        transport_abort(TRANSPORT_DATA_FIRMWARE);
        res->status_code = 400;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"No firmware data received\"}";
        return 0;
    }

    ret = transport_end(TRANSPORT_DATA_FIRMWARE, true);
    if (ret != 0)
    {
        LOG_ERR("transport_end failed: %d", ret);
        res->status_code = 500;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"OTA finalisation failed\"}";
        return 0;
    }

    LOG_INF("FW upload complete: %zu B written", total_written);

    /* Return 200 JSON before reboot so the client gets a response */
    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = "{\"status\":\"rebooting\"}";

    /* Schedule reboot after 3 s to let the response flush */
    ota_reboot_to_apply_update(3000);
    return 0;
#endif /* CONFIG_FLASH_MAP && CONFIG_BOOTLOADER_MCUBOOT */
}

/*===========================================================================*/
/* CORS pre-flight — OPTIONS *                                               */
/*===========================================================================*/

static int route_options(const http_request_t *req, http_response_t *res,
                         void *user_data)
{
    res->status_code = 204;
    res->content_type = HTTP_CONTENT_TEXT;
    res->body = "";
    return 0;
}

/*===========================================================================*/
/* Minimal JSON value extractor (string or bare number/bool token)           */
/*===========================================================================*/

static bool route_json_get(const char *json, const char *key,
                           char *out, size_t out_len)
{
    if (!json)
    {
        return false;
    }
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p)
    {
        return false;
    }
    p += strlen(search);
    while (*p == ' ')
    {
        p++;
    }
    size_t i = 0;
    if (*p == '"')
    {
        p++;
        while (*p && *p != '"' && i < out_len - 1)
        {
            out[i++] = *p++;
        }
    }
    else
    {
        while (*p && *p != ',' && *p != '}' && *p != ' ' && i < out_len - 1)
        {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return true;
}

/*===========================================================================*/
/* GET /api/v1/telemetry — battery / environment snapshot                    */
/*===========================================================================*/

static int route_telemetry(const http_request_t *req, http_response_t *res,
                           void *user_data)
{
    ARG_UNUSED(req);
    ARG_UNUSED(user_data);

    int pos = snprintf(s_route_buf, sizeof(s_route_buf), "{");

#ifdef CONFIG_AKIRA_POWER_MANAGER
    akira_battery_status_t bs;
    if (akira_pm_get_battery_status(&bs) == 0)
    {
        pos += snprintf(s_route_buf + pos, sizeof(s_route_buf) - pos,
                        "\"battery_pct\":%u,\"battery_mv\":%d,"
                        "\"current_ma\":%d,\"charging\":%s",
                        bs.level_percent, bs.voltage_mv, bs.current_ma,
                        bs.charging ? "true" : "false");
    }
#endif

    snprintf(s_route_buf + pos, sizeof(s_route_buf) - pos, "}");

    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
}

/*===========================================================================*/
/* POST /api/v1/power — {"action":"reboot|sleep|poweroff"}                    */
/*                                                                           */
/* The action is deferred to a work item so the HTTP response is flushed     */
/* before the device tears down the link.                                    */
/*===========================================================================*/

enum power_req
{
    POWER_REQ_NONE = 0,
    POWER_REQ_REBOOT,
    POWER_REQ_SLEEP,
    POWER_REQ_POWEROFF,
};
static enum power_req s_power_req;

static void power_action_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    switch (s_power_req)
    {
    case POWER_REQ_REBOOT:
        sys_reboot(SYS_REBOOT_COLD);
        break;
    case POWER_REQ_SLEEP:
#ifdef CONFIG_AKIRA_POWER_MANAGER
        akira_pm_set_mode(POWER_MODE_DEEP_SLEEP);
#endif
        break;
    case POWER_REQ_POWEROFF:
#ifdef CONFIG_POWEROFF
        sys_poweroff();
#else
        sys_reboot(SYS_REBOOT_COLD);
#endif
        break;
    default:
        break;
    }
}
static K_WORK_DELAYABLE_DEFINE(s_power_work, power_action_work_fn);

static int route_power(const http_request_t *req, http_response_t *res,
                       void *user_data)
{
    ARG_UNUSED(user_data);

    char action[16] = {0};
    route_json_get(req->body, "action", action, sizeof(action));

    enum power_req r = POWER_REQ_NONE;
    if (strcmp(action, "reboot") == 0)
    {
        r = POWER_REQ_REBOOT;
    }
    else if (strcmp(action, "sleep") == 0)
    {
        r = POWER_REQ_SLEEP;
    }
    else if (strcmp(action, "poweroff") == 0)
    {
        r = POWER_REQ_POWEROFF;
    }

    if (r == POWER_REQ_NONE)
    {
        res->status_code = 400;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"action must be reboot|sleep|poweroff\"}";
        return 0;
    }

    s_power_req = r;
    /* 300 ms gives http_server.c time to write the response and close. */
    k_work_schedule(&s_power_work, K_MSEC(300));

    snprintf(s_route_buf, sizeof(s_route_buf),
             "{\"status\":\"scheduled\",\"action\":\"%s\"}", action);
    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
}

/*===========================================================================*/
/* GET /api/v1/settings  and  PUT /api/v1/settings                           */
/*                                                                           */
/* Exposes the real NVS-backed device settings used by the OS shell.         */
/*===========================================================================*/

#ifdef CONFIG_AKIRA_SETTINGS

/* Settings surfaced over the API: NVS key <-> JSON field. */
static const struct
{
    const char *key;  /* NVS key             */
    const char *json; /* JSON field name     */
} k_api_settings[] = {
    {"akira/display/brightness", "display_brightness"},
    {"akira/display/timeout_en", "display_timeout_en"},
    {"akira/display/timeout_s", "display_timeout_s"},
    {"akira/power/dispoff_s", "power_dispoff_s"},
    {"akira/power/sleep_s", "power_sleep_s"},
    {"akira/devmode/enabled", "devmode_enabled"},
};

static int route_settings_get(const http_request_t *req, http_response_t *res,
                              void *user_data)
{
    ARG_UNUSED(req);
    ARG_UNUSED(user_data);

    int pos = snprintf(s_route_buf, sizeof(s_route_buf), "{");
    bool first = true;
    char val[32];
    for (size_t i = 0; i < ARRAY_SIZE(k_api_settings); i++)
    {
        if (akira_settings_get(k_api_settings[i].key, val, sizeof(val)) != 0)
        {
            continue;
        }
        pos += snprintf(s_route_buf + pos, sizeof(s_route_buf) - pos,
                        "%s\"%s\":\"%s\"", first ? "" : ",",
                        k_api_settings[i].json, val);
        first = false;
    }
    snprintf(s_route_buf + pos, sizeof(s_route_buf) - pos, "}");

    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
}

static int route_settings_put(const http_request_t *req, http_response_t *res,
                              void *user_data)
{
    ARG_UNUSED(user_data);

    int written = 0;
    char val[32];
    for (size_t i = 0; i < ARRAY_SIZE(k_api_settings); i++)
    {
        if (route_json_get(req->body, k_api_settings[i].json, val, sizeof(val)))
        {
            if (akira_settings_set(k_api_settings[i].key, val, 0) == 0)
            {
                written++;
            }
        }
    }

    snprintf(s_route_buf, sizeof(s_route_buf),
             "{\"status\":\"ok\",\"updated\":%d}", written);
    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
}
#endif /* CONFIG_AKIRA_SETTINGS */

/*===========================================================================*/
/* OTA — GET /api/v1/ota/status, POST /api/v1/ota/upload, /api/v1/ota/confirm */
/*===========================================================================*/

#ifdef AKIRA_HTTP_OTA

static const char *ota_state_to_api(enum ota_state st)
{
    switch (st)
    {
    case OTA_STATE_IDLE:
        return "idle";
    case OTA_STATE_COMPLETE:
        return "pending_confirm";
    case OTA_STATE_ERROR:
        return "error";
    default:
        return "uploading";
    }
}

static int route_ota_status(const http_request_t *req, http_response_t *res,
                            void *user_data)
{
    ARG_UNUSED(req);
    ARG_UNUSED(user_data);

    const struct ota_progress *p = ota_get_progress();
    snprintf(s_route_buf, sizeof(s_route_buf),
             "{\"state\":\"%s\",\"percent\":%u,\"running_version\":\""
             AKIRA_VERSION_STRING "\"}",
             p ? ota_state_to_api(p->state) : "idle",
             p ? p->percentage : 0);

    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = s_route_buf;
    return 0;
}

static int route_ota_upload(const http_request_t *req, http_response_t *res,
                            void *user_data)
{
    ARG_UNUSED(user_data);

    if (!route_check_auth(req))
    {
        res->status_code = 401;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"Unauthorized\"}";
        return 0;
    }

    size_t content_length = req->content_length;
    if (content_length == 0 || content_length > (2U * 1024U * 1024U))
    {
        res->status_code = 400;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"Invalid Content-Length\"}";
        return 0;
    }

    if (ota_start_update(content_length) != OTA_OK)
    {
        res->status_code = 500;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"ota_start failed\"}";
        return 0;
    }

    size_t total_received = 0;
    if (req->body && req->body_len > 0)
    {
        if (ota_write_chunk((const uint8_t *)req->body, req->body_len) != OTA_OK)
        {
            ota_abort_update();
            res->status_code = 500;
            res->content_type = HTTP_CONTENT_JSON;
            res->body = "{\"error\":\"chunk write failed\"}";
            return 0;
        }
        total_received = req->body_len;
    }

    struct akira_buf *buf = akira_buf_alloc(K_MSEC(200));
    if (!buf)
    {
        ota_abort_update();
        res->status_code = 503;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"Server busy\"}";
        return 0;
    }

    struct timeval tv = {.tv_sec = 60, .tv_usec = 0};
    setsockopt(req->client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (total_received < content_length)
    {
        akira_buf_reset(buf);
        size_t want = MIN(AKIRA_BUF_SIZE, content_length - total_received);
        ssize_t got = recv(req->client_fd, buf->data, want, 0);
        if (got <= 0)
        {
            akira_buf_unref(buf);
            ota_abort_update();
            res->status_code = 500;
            res->content_type = HTTP_CONTENT_JSON;
            res->body = "{\"error\":\"Upload incomplete\"}";
            return 0;
        }
        akira_buf_add_len(buf, got);
        if (ota_write_chunk(buf->data, buf->len) != OTA_OK)
        {
            akira_buf_unref(buf);
            ota_abort_update();
            res->status_code = 500;
            res->content_type = HTTP_CONTENT_JSON;
            res->body = "{\"error\":\"chunk write failed\"}";
            return 0;
        }
        total_received += got;
        k_yield();
    }
    akira_buf_unref(buf);

    if (ota_finalize_update() != OTA_OK)
    {
        res->status_code = 500;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = "{\"error\":\"ota_finalize failed\"}";
        return 0;
    }

    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = "{\"status\":\"uploaded\",\"state\":\"pending_confirm\"}";
    return 0;
}

static int route_ota_confirm(const http_request_t *req, http_response_t *res,
                             void *user_data)
{
    ARG_UNUSED(req);
    ARG_UNUSED(user_data);

    enum ota_result r = ota_confirm_firmware();
    if (r != OTA_OK)
    {
        snprintf(s_route_buf, sizeof(s_route_buf),
                 "{\"error\":\"confirm failed: %d\"}", (int)r);
        res->status_code = 500;
        res->content_type = HTTP_CONTENT_JSON;
        res->body = s_route_buf;
        return 0;
    }

    /* Apply by rebooting into the confirmed image shortly after replying. */
    ota_reboot_to_apply_update(300);

    res->status_code = 200;
    res->content_type = HTTP_CONTENT_JSON;
    res->body = "{\"status\":\"applied\"}";
    return 0;
}
#endif /* AKIRA_HTTP_OTA */

/*===========================================================================*/
/* Route table                                                               */
/*===========================================================================*/

int akira_http_routes_init(void)
{
    static const http_route_t routes[] = {
        /* Management API */
        {HTTP_GET, "/api/v1/info", route_info, NULL},
        {HTTP_GET, "/api/v1/status", route_status, NULL},
        {HTTP_GET, "/api/v1/apps", route_apps_list, NULL},
        {HTTP_GET, "/api/v1/logs", route_logs, NULL},
        {HTTP_POST, "/api/v1/apps/start", route_app_start, NULL},
        {HTTP_POST, "/api/v1/apps/stop", route_app_stop, NULL},
        {HTTP_DELETE, "/api/v1/apps", route_app_delete, NULL},
        /* Command centre: telemetry, power, settings, OTA */
        {HTTP_GET, "/api/v1/telemetry", route_telemetry, NULL},
        {HTTP_POST, "/api/v1/power", route_power, NULL},
#ifdef CONFIG_AKIRA_SETTINGS
        {HTTP_GET, "/api/v1/settings", route_settings_get, NULL},
        {HTTP_PUT, "/api/v1/settings", route_settings_put, NULL},
#endif
#ifdef AKIRA_HTTP_OTA
        {HTTP_GET, "/api/v1/ota/status", route_ota_status, NULL},
        {HTTP_POST, "/api/v1/ota/upload", route_ota_upload, NULL},
        {HTTP_POST, "/api/v1/ota/confirm", route_ota_confirm, NULL},
#endif
        /* App + firmware upload */
#if defined(CONFIG_AKIRA_HTTP_DEV_UPLOAD)
        {HTTP_POST, "/api/apps/install", route_app_install, NULL},
#endif
        {HTTP_POST, "/upload", route_firmware_upload, NULL},
        /* CORS */
        {HTTP_OPTIONS, "*", route_options, NULL},
    };

    int ret = 0;
    for (size_t i = 0; i < ARRAY_SIZE(routes); i++)
    {
        ret = akira_http_register_route(&routes[i]);
        if (ret != 0)
        {
            LOG_ERR("Failed to register route %s: %d",
                    routes[i].path, ret);
            return ret;
        }
    }

    LOG_INF("HTTP routes registered (%zu total)", ARRAY_SIZE(routes));
    return 0;
}
