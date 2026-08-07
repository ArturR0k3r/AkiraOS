/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file companion_service.c
 * @brief BLE Companion GATT service implementation.
 *
 * Architecture
 * ------------
 * The service runs in BT_MODE_COMPANION — mutually exclusive with
 * BT_MODE_HID and BT_MODE_BLE_APP.  All BLE callbacks execute in the
 * Zephyr BT thread.  Heavy work (shell execution, file I/O, OTA) is
 * delegated to a dedicated workqueue so the BT thread is never blocked.
 *
 * CMD_CHAR (WRITE) ──► cmd_work (workqueue) ──► handler ──► notify RESP_CHAR
 * DATA_UP  (WRITE_WO_RSP) ──► transfer state machine ──► file/app staging buf
 * STATUS timer ──► companion_svc_notify_status() ──► notify STATUS_CHAR
 *
 * JSON is written/read with the same minimal scanner used in usb_cdc_serial.c.
 * No dynamic JSON library dependencies — keeps the footprint small.
 */

#include "companion_service.h"
#include "bt_manager.h"

#include <zephyr/app_version.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/base64.h>
#include <zephyr/shell/shell_dummy.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "../../lib/mem_helper.h"

LOG_MODULE_REGISTER(companion_svc, CONFIG_AKIRA_LOG_LEVEL);

/* Forward-declared internal headers (pulled in via include paths) */
#include "../../runtime/app_manager/app_manager.h"
#include "../../runtime/app_manager/app_cmd_bridge.h"
#include "../../connectivity/ota/ota_manager.h"
#include "../../settings/settings.h"
#include "../../connectivity/cloud/cloud_app_handler.h"
#include "../../storage/fs_manager.h"

/* --------------------------------------------------------------------------
 * UUID definitions
 * -------------------------------------------------------------------------- */

static struct bt_uuid_128 svc_uuid     = BT_UUID_INIT_128(COMPANION_SVC_UUID);
static struct bt_uuid_128 cmd_uuid     = BT_UUID_INIT_128(COMPANION_CMD_UUID);
static struct bt_uuid_128 resp_uuid    = BT_UUID_INIT_128(COMPANION_RESP_UUID);
static struct bt_uuid_128 data_up_uuid = BT_UUID_INIT_128(COMPANION_DATA_UP_UUID);
static struct bt_uuid_128 data_dn_uuid = BT_UUID_INIT_128(COMPANION_DATA_DOWN_UUID);
static struct bt_uuid_128 status_uuid  = BT_UUID_INIT_128(COMPANION_STATUS_UUID);

/* --------------------------------------------------------------------------
 * Characteristic value buffers
 * -------------------------------------------------------------------------- */

#define CHAR_BUF_SIZE 244

static uint8_t AKIRA_BULK_BSS s_cmd_buf[CHAR_BUF_SIZE];
static uint8_t AKIRA_BULK_BSS s_resp_buf[CHAR_BUF_SIZE];
static uint8_t AKIRA_BULK_BSS s_data_dn_buf[CHAR_BUF_SIZE];
static uint8_t AKIRA_BULK_BSS s_status_buf[CHAR_BUF_SIZE];

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static bool s_initialised;
static struct bt_conn *s_conn;   /* current connection, NULL if not connected */

/* Pending command — written by CMD_CHAR callback, consumed by cmd_work */
static uint8_t AKIRA_BULK_BSS s_pending_cmd[CHAR_BUF_SIZE];
static uint16_t s_pending_cmd_len;
static struct k_spinlock s_cmd_lock;
static struct k_work s_cmd_work;

/* apps.cmd blocks this queue up to APPS_CMD_TIMEOUT_MS waiting on the app's
 * IPC reply — must not run on the system workqueue, or a non-replying app
 * stalls every other subsystem's k_work_submit() for the same duration. */
#define CMD_WORKQ_STACK_SIZE 8192
static K_THREAD_STACK_DEFINE(s_cmd_workq_stack, CMD_WORKQ_STACK_SIZE);
static struct k_work_q s_cmd_workq;

/* Active bulk transfer state */
static struct {
    bool    active;
    uint8_t type;           /* COMP_XFER_APP_DATA / COMP_XFER_FILE_DATA */
    uint8_t *buf;           /* akira_malloc_buffer staging buffer */
    uint32_t capacity;      /* allocated bytes */
    uint32_t received;      /* bytes written so far */
    char     path[128];     /* destination path for COMP_XFER_FILE_DATA */
    char     app_name[32];  /* app name for COMP_XFER_APP_DATA */
    uint32_t expected_size; /* from apps.install.begin size param */
} s_xfer;

/* Periodic status notification timer */
static struct k_work_delayable s_status_timer;

/* --------------------------------------------------------------------------
 * Helpers: minimal JSON tool (same approach as usb_cdc_serial.c)
 * -------------------------------------------------------------------------- */

static bool json_get_str(const char *json, const char *key,
                          char *out, size_t out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) {
        return false;
    }
    p += strlen(search);
    while (*p == ' ') { p++; }

    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < out_len - 1) {
            if (*p == '\\' && *(p + 1)) { p++; }
            out[i++] = *p++;
        }
        out[i] = '\0';
    } else {
        size_t i = 0;
        while (*p && *p != ',' && *p != '}' && i < out_len - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Helpers: send RESP_CHAR notification
 * -------------------------------------------------------------------------- */

static int notify_char(const struct bt_gatt_attr *attr,
                       const uint8_t *data, uint16_t len)
{
    if (!s_conn) {
        return -ENOTCONN;
    }
    return bt_gatt_notify(s_conn, attr, data, len);
}

/* Build and send a JSON response via RESP_CHAR */
static void send_resp(const char *op, int id, bool ok,
                      const char *data_or_error)
{
    int n;
    if (ok) {
        if (data_or_error && data_or_error[0]) {
            n = snprintf((char *)s_resp_buf, sizeof(s_resp_buf),
                         "{\"op\":\"%s\",\"id\":%d,\"ok\":true,\"data\":%s}",
                         op, id, data_or_error);
        } else {
            n = snprintf((char *)s_resp_buf, sizeof(s_resp_buf),
                         "{\"op\":\"%s\",\"id\":%d,\"ok\":true}",
                         op, id);
        }
    } else {
        n = snprintf((char *)s_resp_buf, sizeof(s_resp_buf),
                     "{\"op\":\"%s\",\"id\":%d,\"ok\":false,\"error\":\"%s\"}",
                     op, id, data_or_error ? data_or_error : "error");
    }

    if (n <= 0 || (size_t)n >= sizeof(s_resp_buf)) {
        LOG_ERR("RESP buffer overflow for op=%s", op);
        return;
    }

    /* attr pointer retrieved from the GATT table via the extern below */
    extern const struct bt_gatt_attr *companion_attrs;
    /* RESP_CHAR is at index 3 in the attribute table (svc, cmd_decl, cmd_val,
     * resp_decl, resp_val, resp_ccc, ...) — use bt_gatt_notify with attr=NULL
     * to let Zephyr look it up by CCCD. */
    int rc = bt_gatt_notify(s_conn, &companion_attrs[4], /* resp_val */
                            s_resp_buf, (uint16_t)n);
    if (rc && rc != -ENOTCONN) {
        LOG_WRN("notify RESP failed: %d", rc);
    }
}

/* --------------------------------------------------------------------------
 * Command handlers
 * -------------------------------------------------------------------------- */

static void handle_device_info(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    char buf[CHAR_BUF_SIZE];
    snprintf(buf, sizeof(buf),
             "{\"fw\":\"%s\",\"model\":\"AkiraConsole\","
             "\"bt_addr\":\"<addr>\"}",
             APP_VERSION_STRING);
    send_resp(op, id, true, buf);
}

static void handle_device_reboot(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    send_resp(op, id, true, NULL);
    k_sleep(K_MSEC(200));
    sys_reboot(SYS_REBOOT_COLD);
}

static char AKIRA_BULK_BSS s_list_json[2048];

/* Fragments a buffer across DATA_DOWN notifications (COMP_XFER_FILE_DATA
 * framing) — used when a response exceeds one 244-byte RESP_CHAR notify. */
static void stream_bytes_down(const uint8_t *data, int len)
{
    extern const struct bt_gatt_attr *companion_attrs;
    uint8_t frame[4 + COMP_DATA_PAYLOAD_MAX];
    int off = 0;
    do {
        int chunk = len - off;
        if (chunk > COMP_DATA_PAYLOAD_MAX) {
            chunk = COMP_DATA_PAYLOAD_MAX;
        }
        bool last = (off + chunk >= len);
        frame[0] = COMP_XFER_FILE_DATA;
        frame[1] = last ? COMP_FLAG_LAST : 0;
        frame[2] = (uint8_t)(chunk & 0xFF);
        frame[3] = (uint8_t)((chunk >> 8) & 0xFF);
        if (chunk > 0) {
            memcpy(frame + 4, data + off, chunk);
        }
        if (bt_gatt_notify(s_conn, &companion_attrs[9], frame, (uint16_t)(4 + chunk))) {
            break;
        }
        off += chunk;
        if (!last) {
            k_sleep(K_MSEC(10)); /* let the peer's BLE stack drain */
        }
    } while (off < len);
}

static void handle_apps_list(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    /* The list can exceed one 244-byte notify (many SD-card apps), so ACK on
     * RESP and stream the JSON array over DATA_DOWN. */
    static app_info_t list[32] AKIRA_BULK_BSS;
    int count = app_manager_list(list, (int)(sizeof(list) / sizeof(list[0])));
    if (count < 0) {
        count = 0;
    }

    int pos = 0;
    pos += snprintf(s_list_json + pos, sizeof(s_list_json) - pos, "[");
    for (int i = 0; i < count && (size_t)pos < sizeof(s_list_json) - 96; i++) {
        pos += snprintf(s_list_json + pos, sizeof(s_list_json) - pos,
                        "%s{\"name\":\"%s\",\"state\":\"%s\","
                        "\"version\":\"%s\",\"has_update\":%s,"
                        "\"available_version\":\"%s\"}",
                        i > 0 ? "," : "",
                        list[i].name,
                        app_state_to_str(list[i].state),
                        list[i].version,
                        list[i].has_update ? "true" : "false",
                        list[i].available_version);
    }
    pos += snprintf(s_list_json + pos, sizeof(s_list_json) - pos, "]");

    send_resp(op, id, true, NULL);   /* ACK; array follows on DATA_DOWN */
    stream_bytes_down((const uint8_t *)s_list_json, pos);
}

static void handle_apps_start(const char *op, int id, const char *params)
{
    char name[32] = "";
    json_get_str(params, "name", name, sizeof(name));
    if (!name[0]) {
        send_resp(op, id, false, "missing name");
        return;
    }
    int rc = app_manager_start(name);
    if (rc) {
        char err[32];
        snprintf(err, sizeof(err), "start failed: %d", rc);
        send_resp(op, id, false, err);
    } else {
        send_resp(op, id, true, NULL);
    }
}

static void handle_apps_stop(const char *op, int id, const char *params)
{
    char name[32] = "";
    json_get_str(params, "name", name, sizeof(name));
    if (!name[0]) {
        send_resp(op, id, false, "missing name");
        return;
    }
    int rc = app_manager_stop(name);
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "stop failed");
}

static void handle_apps_uninstall(const char *op, int id, const char *params)
{
    char name[32] = "";
    json_get_str(params, "name", name, sizeof(name));
    if (!name[0]) {
        send_resp(op, id, false, "missing name");
        return;
    }
    int rc = app_manager_uninstall(name);
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "uninstall failed");
}

/* CONFIG_AKIRA_IPC_MSG_MAX_SIZE-bounded: the bridge forwards these bytes
 * straight onto the app's IPC topic, which caps a single message at that
 * Kconfig value. */
#define APPS_CMD_TIMEOUT_MS 3000

struct apps_cmd_ctx {
    char op[16];
    int id;
};

static void apps_cmd_reply_cb(const char *app_name, const void *reply, size_t reply_len,
                              app_cmd_reply_status_t status, void *user_data)
{
    ARG_UNUSED(app_name);
    struct apps_cmd_ctx *ctx = user_data;

    if (status == APP_CMD_REPLY_NOT_LISTENING) {
        send_resp(ctx->op, ctx->id, false, "app not listening");
        return;
    }
    if (status == APP_CMD_REPLY_TIMEOUT) {
        send_resp(ctx->op, ctx->id, false, "timeout");
        return;
    }

    char b64_out[4 * ((CONFIG_AKIRA_IPC_MSG_MAX_SIZE + 2) / 3) + 1];
    size_t b64_len = 0;
    if (base64_encode((uint8_t *)b64_out, sizeof(b64_out), &b64_len, reply, reply_len) != 0) {
        send_resp(ctx->op, ctx->id, false, "reply too large");
        return;
    }
    b64_out[b64_len] = '\0';

    char data[sizeof(b64_out) + 32];
    snprintf(data, sizeof(data), "{\"payload\":\"%s\"}", b64_out);
    send_resp(ctx->op, ctx->id, true, data);
}

static void handle_apps_cmd(const char *op, int id, const char *params)
{
    char name[32] = "";
    char payload_b64[4 * ((CONFIG_AKIRA_IPC_MSG_MAX_SIZE + 2) / 3) + 1] = "";
    json_get_str(params, "name", name, sizeof(name));
    json_get_str(params, "payload", payload_b64, sizeof(payload_b64));

    if (!name[0] || !payload_b64[0]) {
        send_resp(op, id, false, "missing name or payload");
        return;
    }

    uint8_t decoded[CONFIG_AKIRA_IPC_MSG_MAX_SIZE];
    size_t decoded_len = 0;
    if (base64_decode(decoded, sizeof(decoded), &decoded_len,
                      (const uint8_t *)payload_b64, strlen(payload_b64)) != 0) {
        send_resp(op, id, false, "invalid base64 payload");
        return;
    }

    static struct apps_cmd_ctx ctx; /* single connection (CONFIG_BT_MAX_CONN=1) — one in flight */
    strncpy(ctx.op, op, sizeof(ctx.op) - 1);
    ctx.op[sizeof(ctx.op) - 1] = '\0';
    ctx.id = id;

    int rc = app_cmd_bridge_send(name, decoded, decoded_len, apps_cmd_reply_cb, &ctx,
                                 K_MSEC(APPS_CMD_TIMEOUT_MS));
    if (rc < 0 && rc != -ENOENT) {
        /* -ENOENT already answered by the callback (not-listening case) */
        send_resp(op, id, false, "internal error");
    }
}

static void handle_apps_update(const char *op, int id, const char *params)
{
    char name[32] = "";
    json_get_str(params, "name", name, sizeof(name));
    if (!name[0]) {
        send_resp(op, id, false, "missing name");
        return;
    }
    int rc = cloud_app_update(name, NULL, NULL);
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "update failed");
}

static void handle_apps_restart(const char *op, int id, const char *params)
{
    char name[32] = "";
    json_get_str(params, "name", name, sizeof(name));
    if (!name[0]) {
        send_resp(op, id, false, "missing name");
        return;
    }
    int rc = app_manager_restart(name);
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "restart failed");
}

static void handle_apps_install_begin(const char *op, int id, const char *params)
{
    char name[32]  = "";
    char size_s[16] = "";

    json_get_str(params, "name", name, sizeof(name));
    json_get_str(params, "size", size_s, sizeof(size_s));

    if (!name[0] || !size_s[0]) {
        send_resp(op, id, false, "missing name or size");
        return;
    }

    uint32_t size = (uint32_t)strtoul(size_s, NULL, 10);
    uint32_t max  = (uint32_t)CONFIG_AKIRA_BT_COMPANION_MAX_TRANSFER_KB * 1024U;

    if (size == 0 || size > max) {
        send_resp(op, id, false, "invalid size");
        return;
    }

    if (s_xfer.active) {
        /* Abort previous incomplete transfer */
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
    }

    s_xfer.buf = akira_malloc_buffer(size);
    if (!s_xfer.buf) {
        LOG_ERR("OOM for app staging buffer (%u bytes)", size);
        send_resp(op, id, false, "out of memory");
        return;
    }

    s_xfer.type          = COMP_XFER_APP_DATA;
    s_xfer.capacity      = size;
    s_xfer.received      = 0;
    s_xfer.expected_size = size;
    s_xfer.active        = true;
    strncpy(s_xfer.app_name, name, sizeof(s_xfer.app_name) - 1);

    LOG_INF("BLE install begin: app=%s size=%u", name, size);
    send_resp(op, id, true, NULL);
}

static void handle_apps_install_end(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);

    if (!s_xfer.active || s_xfer.type != COMP_XFER_APP_DATA) {
        send_resp(op, id, false, "no active transfer");
        return;
    }

    /* Write staged bytes to a temp file named after the real app — install_akpkg()
     * only overrides this with the embedded manifest's name when that manifest is
     * small enough to parse (see app_manager.c), so the fallback must be correct. */
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "/lfs/tmp/%s.akpkg", s_xfer.app_name);
    fs_manager_mkdir("/lfs/tmp");

    struct fs_file_t f;
    fs_file_t_init(&f);

    int rc = fs_open(&f, tmp_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (rc) {
        LOG_ERR("fs_open %s failed: %d", tmp_path, rc);
        send_resp(op, id, false, "storage error");
        goto cleanup;
    }

    ssize_t written = fs_write(&f, s_xfer.buf, s_xfer.received);
    fs_close(&f);

    if (written < 0 || (uint32_t)written != s_xfer.received) {
        LOG_ERR("fs_write failed: %d", (int)written);
        fs_unlink(tmp_path);
        send_resp(op, id, false, "write error");
        goto cleanup;
    }

    rc = app_manager_install_from_path(tmp_path);
    fs_unlink(tmp_path);

    if (rc) {
        char err[32];
        snprintf(err, sizeof(err), "install failed: %d", rc);
        send_resp(op, id, false, err);
    } else {
        LOG_INF("BLE app install complete: %s", s_xfer.app_name);
        send_resp(op, id, true, NULL);
        companion_svc_notify_status();
    }

cleanup:
    akira_free_buffer(s_xfer.buf);
    memset(&s_xfer, 0, sizeof(s_xfer));
}

static void handle_settings_get(const char *op, int id, const char *params)
{
    char key[64] = "";
    json_get_str(params, "key", key, sizeof(key));
    if (!key[0]) {
        send_resp(op, id, false, "missing key");
        return;
    }
    char val[64] = "";
    int rc = akira_settings_get(key, val, sizeof(val));
    if (rc) {
        send_resp(op, id, false, "not found");
    } else {
        char body[CHAR_BUF_SIZE];
        snprintf(body, sizeof(body), "{\"key\":\"%s\",\"value\":\"%s\"}",
                 key, val);
        send_resp(op, id, true, body);
    }
}

static void handle_settings_set(const char *op, int id, const char *params)
{
    char key[64]   = "";
    char value[64] = "";
    json_get_str(params, "key",   key,   sizeof(key));
    json_get_str(params, "value", value, sizeof(value));
    if (!key[0]) {
        send_resp(op, id, false, "missing key");
        return;
    }
    int rc = akira_settings_set(key, value, 0);
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "set failed");
}

static void handle_settings_list(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    /* Returns compact JSON array of {"key":...,"value":...} entries */
    char key_buf[MAX_KEY_LEN];
    char value_buf[MAX_VALUE_LEN];
    settings_iterator_t iter = {
        .index = 0,
        .count = 0,
        .key = key_buf,
        .value = value_buf,
    };

    char buf[CHAR_BUF_SIZE];
    int pos = snprintf(buf, sizeof(buf), "[");
    bool first = true;
    while ((size_t)pos < sizeof(buf) - 96 && akira_settings_list(&iter) == 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"key\":\"%s\",\"value\":\"%s\"}",
                        first ? "" : ",", iter.key, iter.value);
        first = false;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]");

    send_resp(op, id, true, buf);
}

static void akira_shell_exec(const char *cmd, char *out, size_t out_len)
{
    const struct shell *sh = shell_backend_dummy_get_ptr();

    shell_backend_dummy_clear_output(sh);
    shell_execute_cmd(sh, cmd);

    size_t len = 0;
    const char *output = shell_backend_dummy_get_output(sh, &len);
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, output, len);
    out[len] = '\0';
}

static void handle_shell_exec(const char *op, int id, const char *params)
{
    char cmd[128] = "";
    json_get_str(params, "cmd", cmd, sizeof(cmd));
    if (!cmd[0]) {
        send_resp(op, id, false, "missing cmd");
        return;
    }

    char out[CHAR_BUF_SIZE - 64] = "";
    akira_shell_exec(cmd, out, sizeof(out));

    char body[CHAR_BUF_SIZE];
    snprintf(body, sizeof(body), "{\"output\":\"%s\"}", out);
    send_resp(op, id, true, body);
}

static void handle_files_list(const char *op, int id, const char *params)
{
    char path[128] = "/lfs";
    json_get_str(params, "path", path, sizeof(path));

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);

    int rc = fs_opendir(&dir, path);
    if (rc) {
        send_resp(op, id, false, "opendir failed");
        return;
    }

    char buf[CHAR_BUF_SIZE];
    int pos = snprintf(buf, sizeof(buf), "[");

    struct fs_dirent entry;
    bool first = true;
    while ((rc = fs_readdir(&dir, &entry)) == 0 && entry.name[0]) {
        if ((size_t)pos >= sizeof(buf) - 60) {
            break; /* truncate — client must paginate or list subdirs */
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"name\":\"%s\",\"type\":\"%s\",\"size\":%zu}",
                        first ? "" : ",",
                        entry.name,
                        entry.type == FS_DIR_ENTRY_DIR ? "dir" : "file",
                        entry.size);
        first = false;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]");
    fs_closedir(&dir);

    send_resp(op, id, true, buf);
}

static void handle_files_read(const char *op, int id, const char *params)
{
    char path[128] = "";
    json_get_str(params, "path", path, sizeof(path));
    if (!path[0]) {
        send_resp(op, id, false, "missing path");
        return;
    }

    struct fs_file_t f;
    fs_file_t_init(&f);
    int rc = fs_open(&f, path, FS_O_READ);
    if (rc) {
        send_resp(op, id, false, "open failed");
        return;
    }

    /* Send data in COMP_DATA_PAYLOAD_MAX chunks via DATA_DOWN notifications */
    extern const struct bt_gatt_attr *companion_attrs;
    uint8_t frame[4 + COMP_DATA_PAYLOAD_MAX];
    ssize_t got;
    bool error = false;

    send_resp(op, id, true, NULL); /* ACK — data follows on DATA_DOWN */

    while ((got = fs_read(&f, frame + 4, COMP_DATA_PAYLOAD_MAX)) > 0) {
        uint8_t flags = 0;
        /* Peek ahead: if next read would return 0, this is the last chunk */
        char peek;
        ssize_t more = fs_read(&f, &peek, 1);
        if (more <= 0) {
            flags |= COMP_FLAG_LAST;
        } else {
            /* Seek back one byte */
            fs_seek(&f, -1, FS_SEEK_CUR);
        }

        frame[0] = COMP_XFER_FILE_DATA;
        frame[1] = flags;
        frame[2] = (uint8_t)(got & 0xFF);
        frame[3] = (uint8_t)((got >> 8) & 0xFF);
        int nr = bt_gatt_notify(s_conn, &companion_attrs[10], /* data_dn val */
                                frame, (uint16_t)(4 + got));
        if (nr) {
            error = true;
            break;
        }
        k_sleep(K_MSEC(10)); /* give phone BLE stack time to enqueue */
    }

    if (!error && got < 0) {
        /* Send error frame */
        frame[0] = COMP_XFER_FILE_DATA;
        frame[1] = COMP_FLAG_LAST | COMP_FLAG_ERROR;
        frame[2] = 0;
        frame[3] = 0;
        bt_gatt_notify(s_conn, &companion_attrs[10], frame, 4);
    }

    fs_close(&f);
}

static void handle_files_write(const char *op, int id, const char *params)
{
    char path[128]  = "";
    char size_s[16] = "";
    json_get_str(params, "path", path, sizeof(path));
    json_get_str(params, "size", size_s, sizeof(size_s));

    if (!path[0] || !size_s[0]) {
        send_resp(op, id, false, "missing path or size");
        return;
    }

    uint32_t size = (uint32_t)strtoul(size_s, NULL, 10);
    uint32_t max  = (uint32_t)CONFIG_AKIRA_BT_COMPANION_MAX_TRANSFER_KB * 1024U;

    if (size == 0 || size > max) {
        send_resp(op, id, false, "invalid size");
        return;
    }

    if (s_xfer.active) {
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
    }

    s_xfer.buf = akira_malloc_buffer(size);
    if (!s_xfer.buf) {
        send_resp(op, id, false, "out of memory");
        return;
    }

    s_xfer.type          = COMP_XFER_FILE_DATA;
    s_xfer.capacity      = size;
    s_xfer.received      = 0;
    s_xfer.expected_size = size;
    s_xfer.active        = true;
    strncpy(s_xfer.path, path, sizeof(s_xfer.path) - 1);

    send_resp(op, id, true, NULL);
}

static void handle_files_delete(const char *op, int id, const char *params)
{
    char path[128] = "";
    json_get_str(params, "path", path, sizeof(path));
    if (!path[0]) {
        send_resp(op, id, false, "missing path");
        return;
    }
    int rc = fs_unlink(path);
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "delete failed");
}

static void handle_files_mkdir(const char *op, int id, const char *params)
{
    char path[128] = "";
    json_get_str(params, "path", path, sizeof(path));
    if (!path[0]) {
        send_resp(op, id, false, "missing path");
        return;
    }
    int rc = fs_mkdir(path);
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "mkdir failed");
}

static void handle_ota_start(const char *op, int id, const char *params)
{
    /* TODO: no native (non-WASM) path exists yet to fetch a firmware image
     * from an arbitrary URL+signature and feed it through
     * ota_start_update()/ota_write_chunk()/ota_finalize_update(). The only
     * existing OTA trigger paths are ota_cloud_check_update() (fixed cloud
     * server, no caller-supplied URL) and the WASM-only akira_native_ota_*
     * bridge in akira_ota_api.c. Wire one of those in before enabling this. */
    ARG_UNUSED(params);
    send_resp(op, id, false, "ota over BLE not implemented");
}

static void handle_ota_status(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    char buf[CHAR_BUF_SIZE];
    const struct ota_progress *progress = ota_get_progress();
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"progress\":%u}",
             ota_state_to_string(progress->state), progress->percentage);
    send_resp(op, id, true, buf);
}

/* --------------------------------------------------------------------------
 * Command dispatcher (runs in workqueue context)
 * -------------------------------------------------------------------------- */

static void cmd_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    /* Copy to local buffer — CMD_CHAR may be written again by the time we run */
    char buf[CHAR_BUF_SIZE];
    uint16_t len;

    k_spinlock_key_t key = k_spin_lock(&s_cmd_lock);
    len = s_pending_cmd_len;
    memcpy(buf, s_pending_cmd, len);
    s_pending_cmd_len = 0;
    k_spin_unlock(&s_cmd_lock, key);

    if (len == 0) {
        return;
    }
    buf[len < CHAR_BUF_SIZE ? len : CHAR_BUF_SIZE - 1] = '\0';

    /* Extract op, id, params */
    char op[32]      = "";
    char id_str[16]  = "0";
    char params[180] = "";

    if (!json_get_str(buf, "op", op, sizeof(op))) {
        LOG_WRN("CMD missing 'op' field");
        return;
    }
    json_get_str(buf, "id",     id_str, sizeof(id_str));
    /* params is a nested object — extract the raw substring */
    const char *pp = strstr(buf, "\"params\":");
    if (pp) {
        pp += 9;
        while (*pp == ' ') { pp++; }
        /* Copy up to closing brace (handles flat objects only) */
        if (*pp == '{') {
            size_t depth = 0, i = 0;
            do {
                if (*pp == '{') { depth++; }
                if (*pp == '}') { depth--; }
                if (i < sizeof(params) - 1) { params[i++] = *pp; }
                pp++;
            } while (depth > 0 && *pp);
            params[i] = '\0';
        }
    }

    int id = (int)strtol(id_str, NULL, 10);

    /* Route */
    if      (strcmp(op, COMP_OP_DEVICE_INFO)        == 0) { handle_device_info(op, id, params); }
    else if (strcmp(op, COMP_OP_DEVICE_REBOOT)       == 0) { handle_device_reboot(op, id, params); }
    else if (strcmp(op, COMP_OP_APPS_LIST)           == 0) { handle_apps_list(op, id, params); }
    else if (strcmp(op, COMP_OP_APPS_START)          == 0) { handle_apps_start(op, id, params); }
    else if (strcmp(op, COMP_OP_APPS_STOP)           == 0) { handle_apps_stop(op, id, params); }
    else if (strcmp(op, COMP_OP_APPS_UNINSTALL)      == 0) { handle_apps_uninstall(op, id, params); }
    else if (strcmp(op, COMP_OP_APPS_INSTALL_BEGIN)  == 0) { handle_apps_install_begin(op, id, params); }
    else if (strcmp(op, COMP_OP_APPS_INSTALL_END)    == 0) { handle_apps_install_end(op, id, params); }
    else if (strcmp(op, COMP_OP_APPS_CMD)            == 0) { handle_apps_cmd(op, id, params); }
    else if (strcmp(op, COMP_OP_APPS_UPDATE)         == 0) { handle_apps_update(op, id, params); }
    else if (strcmp(op, COMP_OP_APPS_RESTART)        == 0) { handle_apps_restart(op, id, params); }
    else if (strcmp(op, COMP_OP_SETTINGS_GET)        == 0) { handle_settings_get(op, id, params); }
    else if (strcmp(op, COMP_OP_SETTINGS_SET)        == 0) { handle_settings_set(op, id, params); }
    else if (strcmp(op, COMP_OP_SETTINGS_LIST)       == 0) { handle_settings_list(op, id, params); }
    else if (strcmp(op, COMP_OP_SHELL_EXEC)          == 0) { handle_shell_exec(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_LIST)          == 0) { handle_files_list(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_READ)          == 0) { handle_files_read(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_WRITE)         == 0) { handle_files_write(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_DELETE)        == 0) { handle_files_delete(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_MKDIR)         == 0) { handle_files_mkdir(op, id, params); }
    else if (strcmp(op, COMP_OP_OTA_START)           == 0) { handle_ota_start(op, id, params); }
    else if (strcmp(op, COMP_OP_OTA_STATUS)          == 0) { handle_ota_status(op, id, params); }
    else {
        char err[48];
        snprintf(err, sizeof(err), "unknown op: %.*s", 32, op);
        send_resp(op, id, false, err);
    }
}

/* --------------------------------------------------------------------------
 * GATT Attribute read/write callbacks
 * -------------------------------------------------------------------------- */

static ssize_t cmd_write(struct bt_conn *conn,
                          const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len,
                          uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset); ARG_UNUSED(flags);

    if (len == 0 || len > CHAR_BUF_SIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    k_spinlock_key_t irq_key = k_spin_lock(&s_cmd_lock);
    memcpy(s_pending_cmd, buf, len);
    s_pending_cmd_len = len;
    k_spin_unlock(&s_cmd_lock, irq_key);

    k_work_submit_to_queue(&s_cmd_workq, &s_cmd_work);
    return (ssize_t)len;
}

static ssize_t data_up_write(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len,
                              uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset); ARG_UNUSED(flags);

    if (len < 4) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const uint8_t *frame = (const uint8_t *)buf;
    /* uint8_t type  = frame[0]; */
    uint8_t  xflags = frame[1];
    uint16_t plen   = (uint16_t)(frame[2] | ((uint16_t)frame[3] << 8));

    if ((uint16_t)len < 4 + plen) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (!s_xfer.active) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }

    /* Copy payload into staging buffer */
    if (s_xfer.received + plen > s_xfer.capacity) {
        LOG_ERR("Transfer overflow: received %u + %u > capacity %u",
                s_xfer.received, plen, s_xfer.capacity);
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }

    memcpy(s_xfer.buf + s_xfer.received, frame + 4, plen);
    s_xfer.received += plen;

    if (xflags & COMP_FLAG_ERROR) {
        LOG_WRN("Transfer aborted by peer");
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
        return (ssize_t)len;
    }

    if (xflags & COMP_FLAG_LAST) {
        /* Transfer complete — for file writes, flush to filesystem now */
        if (s_xfer.type == COMP_XFER_FILE_DATA) {
            struct fs_file_t f;
            fs_file_t_init(&f);
            int rc = fs_open(&f, s_xfer.path,
                             FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
            if (rc == 0) {
                rc = (int)fs_write(&f, s_xfer.buf, s_xfer.received);
                fs_close(&f);
            }
            LOG_INF("BLE file write %s: %s (%u bytes)",
                    s_xfer.path,
                    rc >= 0 ? "OK" : "FAIL",
                    s_xfer.received);
            akira_free_buffer(s_xfer.buf);
            memset(&s_xfer, 0, sizeof(s_xfer));
        }
        /* For COMP_XFER_APP_DATA, leave s_xfer active — apps.install.end
         * uses s_xfer.buf/received and cleans up itself. */
    }

    return (ssize_t)len;
}

static ssize_t status_read(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset)
{
    ARG_UNUSED(conn); ARG_UNUSED(attr);
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             s_status_buf, strlen((char *)s_status_buf));
}

static void resp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    LOG_INF("RESP_CHAR CCC: %s", value == BT_GATT_CCC_NOTIFY ? "notify" : "off");
}

static void data_dn_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    LOG_INF("DATA_DOWN CCC: %s", value == BT_GATT_CCC_NOTIFY ? "notify" : "off");
}

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    LOG_INF("STATUS CCC: %s", value == BT_GATT_CCC_NOTIFY ? "notify" : "off");
    if (value == BT_GATT_CCC_NOTIFY) {
        companion_svc_notify_status(); /* push immediately on subscribe */
    }
}

/* --------------------------------------------------------------------------
 * GATT service table
 * -------------------------------------------------------------------------- */

/* Attribute indices used by notify helpers above:
 *  0  — service declaration
 *  1  — CMD_CHAR declaration
 *  2  — CMD_CHAR value    (cmd_write)
 *  3  — RESP_CHAR declaration
 *  4  — RESP_CHAR value
 *  5  — RESP_CHAR CCC
 *  6  — DATA_UP declaration
 *  7  — DATA_UP value    (data_up_write)
 *  8  — DATA_DOWN declaration
 *  9  — DATA_DOWN value
 * 10  — DATA_DOWN CCC
 * 11  — STATUS declaration
 * 12  — STATUS value     (status_read)
 * 13  — STATUS CCC
 */

BT_GATT_SERVICE_DEFINE(companion_svc_def,
    BT_GATT_PRIMARY_SERVICE(&svc_uuid),

    /* CMD_CHAR: WRITE (carries shell/fs/app/OTA commands). No encryption
     * requirement — iOS CoreBluetooth sets AuthReq.MITM=1 on any pairing it
     * triggers for an encrypted characteristic, and our NoInputNoOutput IO
     * capability can only do Just Works, which can never satisfy MITM. That
     * made every phone connection loop on AUTH_REQUIREMENT and disconnect. */
    BT_GATT_CHARACTERISTIC(&cmd_uuid.uuid,
                            BT_GATT_CHRC_WRITE,
                            BT_GATT_PERM_WRITE,
                            NULL, cmd_write, s_cmd_buf),

    /* RESP_CHAR: NOTIFY */
    BT_GATT_CHARACTERISTIC(&resp_uuid.uuid,
                            BT_GATT_CHRC_NOTIFY,
                            BT_GATT_PERM_NONE,
                            NULL, NULL, s_resp_buf),
    BT_GATT_CCC(resp_ccc_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* DATA_UP: WRITE_WITHOUT_RSP (file upload). No encryption requirement —
     * see CMD_CHAR above. */
    BT_GATT_CHARACTERISTIC(&data_up_uuid.uuid,
                            BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                            BT_GATT_PERM_WRITE,
                            NULL, data_up_write, NULL),

    /* DATA_DOWN: NOTIFY */
    BT_GATT_CHARACTERISTIC(&data_dn_uuid.uuid,
                            BT_GATT_CHRC_NOTIFY,
                            BT_GATT_PERM_NONE,
                            NULL, NULL, s_data_dn_buf),
    BT_GATT_CCC(data_dn_ccc_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* STATUS: READ + NOTIFY */
    BT_GATT_CHARACTERISTIC(&status_uuid.uuid,
                            BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                            BT_GATT_PERM_READ,
                            status_read, NULL, s_status_buf),
    BT_GATT_CCC(status_ccc_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* Export attribute table pointer used by notify helpers */
const struct bt_gatt_attr *companion_attrs = companion_svc_def.attrs;

/* --------------------------------------------------------------------------
 * Connection tracking callbacks
 * -------------------------------------------------------------------------- */

static void conn_cb_connected(struct bt_conn *conn, uint8_t err)
{
    if (err || bt_manager_get_mode() != BT_MODE_COMPANION) {
        return;
    }
    s_conn = bt_conn_ref(conn);
    LOG_INF("Companion: phone connected");
    companion_svc_notify_status();
}

static void conn_cb_disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (s_conn != conn) {
        return;
    }
    bt_conn_unref(s_conn);
    s_conn = NULL;
    LOG_INF("Companion: phone disconnected (reason 0x%02x)", reason);

    /* Clean up any in-progress transfer */
    if (s_xfer.active) {
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
    }
}

BT_CONN_CB_DEFINE(companion_conn_cb) = {
    .connected    = conn_cb_connected,
    .disconnected = conn_cb_disconnected,
};

/* --------------------------------------------------------------------------
 * Periodic status timer
 * -------------------------------------------------------------------------- */

static void status_timer_handler(struct k_work *work)
{
    companion_svc_notify_status();
    k_work_reschedule(&s_status_timer,
                      K_MSEC(CONFIG_AKIRA_BT_COMPANION_STATUS_INTERVAL_MS));
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int companion_svc_init(void)
{
    if (s_initialised) {
        return -EALREADY;
    }

    int rc = bt_manager_set_mode(BT_MODE_COMPANION);
    if (rc) {
        LOG_ERR("Cannot acquire BT_MODE_COMPANION: %d", rc);
        return rc;
    }

    k_work_queue_init(&s_cmd_workq);
    k_work_queue_start(&s_cmd_workq, s_cmd_workq_stack,
                       K_THREAD_STACK_SIZEOF(s_cmd_workq_stack),
                       K_PRIO_PREEMPT(7), NULL);

    k_work_init(&s_cmd_work, cmd_work_handler);
    k_work_init_delayable(&s_status_timer, status_timer_handler);

    /* Start periodic status notifications */
    k_work_reschedule(&s_status_timer,
                      K_MSEC(CONFIG_AKIRA_BT_COMPANION_STATUS_INTERVAL_MS));

    s_initialised = true;
    LOG_INF("Companion GATT service initialised");

    /* Start advertising with companion service UUID */
    static const uint8_t companion_uuid128[16] = {
        /* A1524C02-0001-4E56-8D4E-494B52413001 in LE byte order */
        0x01, 0x30, 0x41, 0x52, 0x4B, 0x49, 0x4E, 0x8D,
        0x56, 0x4E, 0x01, 0x00, 0x02, 0x4C, 0x52, 0xA1
    };
    bt_manager_start_advertising_custom(companion_uuid128);

    return 0;
}

int companion_svc_deinit(void)
{
    if (!s_initialised) {
        return -EALREADY;
    }

    k_work_cancel_delayable(&s_status_timer);

    if (s_conn) {
        bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(s_conn);
        s_conn = NULL;
    }

    if (s_xfer.active) {
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
    }

    bt_manager_set_mode(BT_MODE_NONE);
    s_initialised = false;
    LOG_INF("Companion GATT service deinitialised");
    return 0;
}

void companion_svc_notify_status(void)
{
    if (!s_conn) {
        return;
    }

    /* running_apps: pulled from the app_manager registry. Heap-allocate the
     * listing buffer — app_info_t carries a 512B commands_json field per
     * entry, too large for a stack array on threads that call this function
     * (BT RX WQ runs near its stack ceiling already). */
    char running_apps_json[64] = "[]";
    app_info_t *apps = akira_malloc_buffer(sizeof(app_info_t) * CONFIG_AKIRA_APP_MAX_INSTALLED);
    if (apps) {
        int count = app_manager_list(apps, CONFIG_AKIRA_APP_MAX_INSTALLED);
        size_t off = 0;
        bool first = true;
        running_apps_json[off++] = '[';
        for (int i = 0; i < count && off < sizeof(running_apps_json) - 3; i++) {
            if (apps[i].state != APP_STATE_RUNNING) {
                continue;
            }
            int written = snprintf(running_apps_json + off, sizeof(running_apps_json) - off,
                                    "%s\"%s\"", first ? "" : ",", apps[i].name);
            if (written < 0 || (size_t)written >= sizeof(running_apps_json) - off) {
                break;
            }
            off += (size_t)written;
            first = false;
        }
        running_apps_json[off++] = ']';
        running_apps_json[off] = '\0';
        akira_free_buffer(apps);
    }

    /* Build the same JSON as cloud device.status.
     * free_heap: TODO — k_mem_slab_num_free_get() takes a specific slab, not
     * a global query; the system heap object isn't part of Zephyr's public
     * API here. Calling it with NULL is a provable null-deref, and GCC was
     * silently compiling this whole function down to a trap instruction
     * because of it. Reporting 0 until a real heap-stats source is wired. */
    int n = snprintf((char *)s_status_buf, sizeof(s_status_buf),
                     "{\"fw\":\"%s\",\"free_heap\":%u,"
                     "\"running_apps\":%s,\"bt_rssi\":0}",
                     APP_VERSION_STRING, 0U, running_apps_json);

    if (n <= 0 || (size_t)n >= sizeof(s_status_buf)) {
        return;
    }

    extern const struct bt_gatt_attr *companion_attrs;
    int rc = bt_gatt_notify(s_conn, &companion_attrs[12], /* status val */
                            s_status_buf, (uint16_t)n);
    LOG_INF("status notify rc=%d len=%d buf=%s", rc, n, s_status_buf);
}

bool companion_svc_is_ready(void)
{
    return s_initialised;
}

/* --------------------------------------------------------------------------
 * SYS_INIT auto-start when CONFIG_AKIRA_BT_COMPANION=y
 * -------------------------------------------------------------------------- */

#if defined(CONFIG_AKIRA_BT_COMPANION)
static int companion_auto_init(void)
{
    return companion_svc_init();
}
SYS_INIT(companion_auto_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif
