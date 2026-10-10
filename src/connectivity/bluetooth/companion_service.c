/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
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
 * delegated to the system workqueue so the BT thread is never blocked.
 * That queue also runs the watchdog feeder, so a handler must not wait long
 * on it; slow operations answer later from a delayable work item.
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

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/reboot.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "../../lib/mem_helper.h"
#include "../../storage/sd_card.h"
#include "companion_fs_util.h"
#include "../../console_shell/akira_os_shell.h"
#ifdef CONFIG_AKIRA_USB_MSC
#include "../../storage/usb_msc.h"
#define SD_IN_MSC() akira_usb_msc_owns_sd()
#else
#define SD_IN_MSC() false
#endif

LOG_MODULE_REGISTER(companion_svc, CONFIG_AKIRA_LOG_LEVEL);

/* Forward-declared internal headers (pulled in via include paths) */
#include "../../runtime/app_manager/app_manager.h"
#include <lib/akpkg.h>
#ifdef CONFIG_AKIRA_OTA
#include "../../connectivity/ota/ota_manager.h"
#include <mbedtls/sha256.h>
#endif
#include <settings/settings.h>
#include <akira.h>

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

static uint8_t s_cmd_buf[CHAR_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
static uint8_t s_resp_buf[CHAR_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
static uint8_t s_data_dn_buf[CHAR_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
static uint8_t s_status_buf[CHAR_BUF_SIZE] __attribute__((section(".ext_ram.bss")));

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static bool s_initialised;
static struct bt_conn *s_conn;   /* current connection, NULL if not connected */

/* Pending command — written by CMD_CHAR callback, consumed by cmd_work */
static uint8_t s_pending_cmd[CHAR_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
static uint16_t s_pending_cmd_len;
static struct k_work s_cmd_work;

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

/* True while the phone holds the SD card for the file explorer (see fs_session_*). */
static bool s_fs_session;

#ifdef CONFIG_AKIRA_OTA
/* Firmware image being streamed to the OTA slot: running hash and the digest
 * the phone announced in ota.begin. */
static mbedtls_sha256_context s_fw_sha;
static uint8_t s_fw_expected[32];

static void fw_abort(void);
#endif

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
             AKIRA_VERSION_STRING);
    send_resp(op, id, true, buf);
}

static void handle_device_reboot(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    send_resp(op, id, true, NULL);
    k_sleep(K_MSEC(200));
    sys_reboot(SYS_REBOOT_COLD);
}

/* Stream a byte buffer to the peer over DATA_DOWN in <=240-byte frames. Used
 * for payloads that exceed the single-notify RESP limit (e.g. apps.list on a
 * device with many apps). Sends at least one frame (LAST set on the final one).
 * Staging buffers live in PSRAM (.ext_ram.bss) to spare internal DRAM. */
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

static char s_list_json[2048] __attribute__((section(".ext_ram.bss")));

static void handle_apps_list(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    /* The list can exceed one 244-byte notify (many SD-card apps), so ACK on
     * RESP and stream the JSON array over DATA_DOWN. */
    static app_info_t list[32] __attribute__((section(".ext_ram.bss")));
    int count = app_manager_list(list, (int)(sizeof(list) / sizeof(list[0])));
    if (count < 0) {
        count = 0;
    }

    int pos = 0;
    pos += snprintf(s_list_json + pos, sizeof(s_list_json) - pos, "[");
    for (int i = 0; i < count && (size_t)pos < sizeof(s_list_json) - 96; i++) {
        pos += snprintf(s_list_json + pos, sizeof(s_list_json) - pos,
                        "%s{\"name\":\"%s\",\"state\":\"%s\","
                        "\"version\":\"%s\"}",
                        i > 0 ? "," : "",
                        list[i].name,
                        app_state_to_str(list[i].state),
                        list[i].version);
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

static void handle_apps_install_begin(const char *op, int id, const char *params)
{
    if (s_fs_session) {
        send_resp(op, id, false, "busy");
        return;
    }
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

#ifdef CONFIG_AKIRA_OTA
    fw_abort();
#endif
    if (s_xfer.active) {
        /* Abort previous incomplete transfer */
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
        akira_sd_card_set_transfer_active(false);
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
    akira_sd_card_set_transfer_active(true);
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

    /* Install straight from the staged buffer, dispatching on format exactly
     * as bt_app_transfer.c does.
     *
     * This used to write the bytes to /lfs/tmp/ble_upload.akpkg and call
     * app_manager_install_from_path(), which broke two ways: nothing in the
     * tree creates /lfs/tmp (so fs_open returned -ENOENT), and that path
     * validates raw WASM/AOT magic — a real .akpkg is gzip, so it failed with
     * "Invalid WASM/AOT magic". Installing from memory needs no temp file and
     * handles both formats. */
    int rc;

    if (akpkg_is_gzip(s_xfer.buf, s_xfer.received)) {
        LOG_INF("BLE install: .akpkg (gzip) format, %u bytes", s_xfer.received);
        char name_buf[APP_NAME_MAX_LEN];
        strncpy(name_buf, s_xfer.app_name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        rc = app_manager_install_akpkg(name_buf, sizeof(name_buf),
                                       s_xfer.buf, s_xfer.received,
                                       APP_SOURCE_BLE);
        /* app_manager_install_akpkg() lets the package manifest's "name" win
         * over the caller-supplied one and writes the resolved name back into
         * name_buf.  Copy it into the transfer state (as bt_app_transfer.c
         * does) so the log, the status notification and the response to the
         * phone all report the app's real name rather than the placeholder
         * the phone sent at install.begin. */
        strncpy(s_xfer.app_name, name_buf, sizeof(s_xfer.app_name) - 1);
        s_xfer.app_name[sizeof(s_xfer.app_name) - 1] = '\0';
    } else {
        LOG_INF("BLE install: raw WASM/AOT, %u bytes", s_xfer.received);
        rc = app_manager_install(s_xfer.app_name, s_xfer.buf, s_xfer.received,
                                 NULL, APP_SOURCE_BLE);
    }

    if (rc < 0) {
        char err[32];
        snprintf(err, sizeof(err), "install failed: %d", rc);
        LOG_ERR("BLE app install failed: %d", rc);
        send_resp(op, id, false, err);
    } else {
        LOG_INF("BLE app install complete: %s (id=%d)", s_xfer.app_name, rc);
        /* Hand the resolved name back so the phone can show what actually got
         * installed — install.end previously answered with no data at all. */
        char done[96];
        snprintf(done, sizeof(done), "{\"name\":\"%s\",\"id\":%d}",
                 s_xfer.app_name, rc);
        send_resp(op, id, true, done);
        companion_svc_notify_status();
    }

    /* Staged buffer is owned here now that data_up_write() no longer frees it
     * for app transfers — release it on every path. */
    akira_free_buffer(s_xfer.buf);
    memset(&s_xfer, 0, sizeof(s_xfer));
    akira_sd_card_set_transfer_active(false);
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
    int rc = akira_settings_set(key, value, 0 /* not encrypted */);
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "set failed");
}

static void handle_settings_list(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    /* No bulk enumeration API in NVS settings; return the known device keys
     * (mirrors the HTTP /api/v1/settings surface) as [{key,value}, ...]. */
    static const char *const keys[] = {
        "akira/display/brightness",
        "akira/display/timeout_en",
        "akira/display/timeout_s",
        "akira/power/sleep_s",
        "akira/power/dispoff_s",
        "akira/devmode/enabled",
    };

    char buf[CHAR_BUF_SIZE];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "[");
    bool first = true;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        char val[48] = "";
        if (akira_settings_get(keys[i], val, sizeof(val)) != 0) {
            continue; /* unset — skip */
        }
        if ((size_t)pos >= sizeof(buf) - 80) {
            break;
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"key\":\"%s\",\"value\":\"%s\"}",
                        first ? "" : ",", keys[i], val);
        first = false;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]");
    send_resp(op, id, true, buf);
}

static void handle_shell_exec(const char *op, int id, const char *params)
{
    char cmd[128] = "";
    json_get_str(params, "cmd", cmd, sizeof(cmd));
    if (!cmd[0]) {
        send_resp(op, id, false, "missing cmd");
        return;
    }

    /* No shell-exec-with-output implementation exists in this firmware, so
     * remote shell over BLE is not supported. */
    send_resp(op, id, false, "shell exec not supported");
}

/* --------------------------------------------------------------------------
 * SD file-explorer session: one at a time, lease-based, dropped on disconnect.
 * -------------------------------------------------------------------------- */

#define FS_LEASE_MS 30000

static struct k_work_delayable s_fs_lease;

static void fs_session_release(void)
{
    if (!s_fs_session) {
        return;
    }
    s_fs_session = false;
    k_work_cancel_delayable(&s_fs_lease);
    if (s_xfer.active && s_xfer.type == COMP_XFER_FILE_DATA) {
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
        akira_sd_card_set_transfer_active(false);
    }
    akira_sd_card_set_fs_session(false);
}

static void fs_lease_expired(struct k_work *work)
{
    ARG_UNUSED(work);
    LOG_WRN("fs session lease expired");
    fs_session_release();
}

/* Every files.* op calls this first. Renews the lease. */
static bool fs_gate(const char *op, int id)
{
    if (!s_fs_session) {
        send_resp(op, id, false, "no_session");
        return false;
    }
    if (SD_IN_MSC()) {
        send_resp(op, id, false, "sd_in_usb_mode");
        return false;
    }
    k_work_reschedule(&s_fs_lease, K_MSEC(FS_LEASE_MS));
    return true;
}

#define FS_APPS_DIR "/SD:/apps/"

static bool fs_in_apps(const char *path)
{
    return strncmp(path, FS_APPS_DIR, strlen(FS_APPS_DIR)) == 0;
}

/* True when path is the binary or manifest of an app that is running. */
static bool fs_app_running(const char *path)
{
    if (!fs_in_apps(path)) {
        return false;
    }
    char name[APP_NAME_MAX_LEN];
    strncpy(name, path + strlen(FS_APPS_DIR), sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *dot = strrchr(name, '.');
    if (dot) {
        *dot = '\0';
    }
    return app_manager_get_state(name) == APP_STATE_RUNNING;
}

/* Keep the app registry and launcher in step with /SD:/apps changes. */
static void fs_apps_changed(const char *path)
{
    if (fs_in_apps(path)) {
        app_manager_resync_sd_apps();
        akira_os_shell_notify_app_changed();
    }
}

#define FS_LIST_BODY_MAX 150   /* leaves room for the "next" tail and the RESP envelope in 244 B */
#define FS_READ_MAX      2048

static uint8_t s_fs_read_buf[FS_READ_MAX] __attribute__((section(".ext_ram.bss")));

static void handle_files_list(const char *op, int id, const char *params)
{
    char path[COMP_FS_PATH_MAX] = COMP_FS_ROOT;
    char cursor_s[12] = "0";

    if (!fs_gate(op, id)) {
        return;
    }
    json_get_str(params, "path", path, sizeof(path));
    json_get_str(params, "cursor", cursor_s, sizeof(cursor_s));
    if (!comp_fs_path_ok(path)) {
        send_resp(op, id, false, "path_denied");
        return;
    }
    long cursor = strtol(cursor_s, NULL, 10);

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);
    if (fs_opendir(&dir, path)) {
        send_resp(op, id, false, "not_found");
        return;
    }

    char buf[FS_LIST_BODY_MAX + 40];
    int pos = snprintf(buf, sizeof(buf), "{\"entries\":[");
    int next = -1;
    long idx = 0;
    bool first = true;
    struct fs_dirent entry;

    while (fs_readdir(&dir, &entry) == 0 && entry.name[0]) {
        if (idx++ < cursor) {
            continue;
        }
        char item[96];
        int n = comp_fs_fmt_entry(item, sizeof(item), entry.name,
                                  entry.type == FS_DIR_ENTRY_DIR, entry.size);
        if (n < 0) {
            continue;                       /* name too long for a page: skip it */
        }
        if (pos + n + 1 > FS_LIST_BODY_MAX) {
            next = (int)(idx - 1);          /* this entry starts the next page */
            break;
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s", first ? "" : ",", item);
        first = false;
    }
    fs_closedir(&dir);
    snprintf(buf + pos, sizeof(buf) - pos, "],\"next\":%d}", next);
    send_resp(op, id, true, buf);
}

static void handle_files_read(const char *op, int id, const char *params)
{
    char path[COMP_FS_PATH_MAX] = "";
    char off_s[12] = "0";
    char len_s[8] = "2048";

    if (!fs_gate(op, id)) {
        return;
    }
    json_get_str(params, "path", path, sizeof(path));
    json_get_str(params, "offset", off_s, sizeof(off_s));
    json_get_str(params, "len", len_s, sizeof(len_s));
    if (!comp_fs_path_ok(path)) {
        send_resp(op, id, false, "path_denied");
        return;
    }
    uint32_t offset = (uint32_t)strtoul(off_s, NULL, 10);
    uint32_t want = (uint32_t)strtoul(len_s, NULL, 10);
    if (want == 0 || want > FS_READ_MAX) {
        want = FS_READ_MAX;
    }

    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, path, FS_O_READ)) {
        send_resp(op, id, false, "not_found");
        return;
    }
    ssize_t got = 0;
    int rc = fs_seek(&f, offset, FS_SEEK_SET);
    if (rc == 0) {
        got = fs_read(&f, s_fs_read_buf, want);
    }
    fs_close(&f);
    if (rc || got < 0) {
        send_resp(op, id, false, "io_error");
        return;
    }

    char data[24];
    snprintf(data, sizeof(data), "{\"n\":%d}", (int)got);
    send_resp(op, id, true, data);
    if (got > 0) {
        stream_bytes_down(s_fs_read_buf, (int)got);
    }
}

static void handle_files_write(const char *op, int id, const char *params)
{
    char path[COMP_FS_PATH_MAX] = "";
    char size_s[16] = "";

    if (!fs_gate(op, id)) {
        return;
    }
    json_get_str(params, "path", path, sizeof(path));
    json_get_str(params, "size", size_s, sizeof(size_s));
    if (!comp_fs_path_mutable_ok(path)) {
        send_resp(op, id, false, "path_denied");
        return;
    }
    if (!size_s[0]) {
        send_resp(op, id, false, "missing size");
        return;
    }

    uint32_t size = (uint32_t)strtoul(size_s, NULL, 10);
    uint32_t max  = (uint32_t)CONFIG_AKIRA_BT_COMPANION_MAX_TRANSFER_KB * 1024U;
    if (size == 0 || size > max) {
        send_resp(op, id, false, "invalid size");
        return;
    }
    if (s_xfer.active && s_xfer.type != COMP_XFER_FILE_DATA) {
        send_resp(op, id, false, "busy");
        return;
    }
    if (s_xfer.active) {                    /* restart of an earlier, unfinished upload */
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

static void handle_files_write_end(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    if (!fs_gate(op, id)) {
        return;
    }
    if (!s_xfer.active || s_xfer.type != COMP_XFER_FILE_DATA) {
        send_resp(op, id, false, "no active transfer");
        return;
    }

    if (fs_app_running(s_xfer.path)) {
        send_resp(op, id, false, "busy");
        return;
    }
    int rc = (s_xfer.received == s_xfer.expected_size) ? 0 : -EIO;
    char tmp[COMP_FS_PATH_MAX];
    if (rc == 0) {
        rc = comp_fs_tmp_path(s_xfer.path, tmp, sizeof(tmp));
    }
    if (rc == 0) {
        struct fs_file_t f;
        fs_file_t_init(&f);
        rc = fs_open(&f, tmp, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
        if (rc == 0) {
            ssize_t w = fs_write(&f, s_xfer.buf, s_xfer.received);
            int c = fs_close(&f);
            rc = (w == (ssize_t)s_xfer.received && c == 0) ? 0 : -EIO;
        }
        if (rc == 0) {
            fs_unlink(s_xfer.path);         /* FAT cannot rename over an existing file */
            rc = fs_rename(tmp, s_xfer.path);
        }
        if (rc != 0) {
            fs_unlink(tmp);
        }
    }
    LOG_INF("BLE file write %s: %s (%u bytes)", s_xfer.path,
            rc == 0 ? "OK" : "FAIL", s_xfer.received);
    if (rc == 0) {
        fs_apps_changed(s_xfer.path);
    }

    akira_free_buffer(s_xfer.buf);
    memset(&s_xfer, 0, sizeof(s_xfer));
    akira_sd_card_set_transfer_active(false);
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "io_error");
}

static void handle_files_delete(const char *op, int id, const char *params)
{
    char path[COMP_FS_PATH_MAX] = "";
    if (!fs_gate(op, id)) {
        return;
    }
    json_get_str(params, "path", path, sizeof(path));
    if (!comp_fs_path_mutable_ok(path)) {
        send_resp(op, id, false, "path_denied");
        return;
    }
    if (fs_app_running(path)) {
        send_resp(op, id, false, "busy");
        return;
    }
    int rc = fs_unlink(path);
    if (rc == 0) {
        fs_apps_changed(path);
    }
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "io_error");
}

static void handle_files_mkdir(const char *op, int id, const char *params)
{
    char path[COMP_FS_PATH_MAX] = "";
    if (!fs_gate(op, id)) {
        return;
    }
    json_get_str(params, "path", path, sizeof(path));
    if (!comp_fs_path_mutable_ok(path)) {
        send_resp(op, id, false, "path_denied");
        return;
    }
    int rc = fs_mkdir(path);
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "io_error");
}

static void handle_files_rename(const char *op, int id, const char *params)
{
    char from[COMP_FS_PATH_MAX] = "";
    char to[COMP_FS_PATH_MAX] = "";
    if (!fs_gate(op, id)) {
        return;
    }
    json_get_str(params, "from", from, sizeof(from));
    json_get_str(params, "to", to, sizeof(to));
    if (!comp_fs_path_mutable_ok(from) || !comp_fs_path_mutable_ok(to)) {
        send_resp(op, id, false, "path_denied");
        return;
    }
    if (fs_app_running(from) || fs_app_running(to)) {
        send_resp(op, id, false, "busy");
        return;
    }
    int rc = fs_rename(from, to);
    if (rc == 0) {
        fs_apps_changed(from);
        fs_apps_changed(to);
    }
    send_resp(op, id, rc == 0, rc == 0 ? NULL : "io_error");
}

#ifdef CONFIG_AKIRA_OTA

#define FW_ERASE_TIMEOUT_MS 60000
#define FW_ERASE_POLL_MS    100

/* ota.begin is answered from s_begin_work, not from the command handler: the
 * slot erase takes seconds, and waiting for it on the system workqueue starves
 * the watchdog feeder and the UI. */
static struct k_work_delayable s_begin_work;
static bool s_begin_pending;
static int s_begin_id;
static uint32_t s_begin_size;
static int64_t s_begin_deadline;

static void fw_abort(void)
{
    if (s_begin_pending) {
        s_begin_pending = false;
        k_work_cancel_delayable(&s_begin_work);
        ota_abort_update();
    }
    if (s_xfer.active && s_xfer.type == COMP_XFER_FW_DATA) {
        mbedtls_sha256_free(&s_fw_sha);
        ota_abort_update();
        memset(&s_xfer, 0, sizeof(s_xfer));
        akira_sd_card_set_transfer_active(false);
    }
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        char pair[3] = {hex[2 * i], hex[2 * i + 1], '\0'};
        char *endp;
        unsigned long v = strtoul(pair, &endp, 16);
        if (*endp != '\0') {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

static void handle_ota_begin(const char *op, int id, const char *params)
{
    if (s_fs_session) {
        send_resp(op, id, false, "busy");
        return;
    }
    char size_s[16] = {0};
    char sha_s[72] = {0};
    json_get_str(params, "size", size_s, sizeof(size_s));
    json_get_str(params, "sha256", sha_s, sizeof(sha_s));

    uint32_t size = (uint32_t)strtoul(size_s, NULL, 10);
    size_t primary = 0, secondary = 0;
    if (size == 0 || !hex_to_bytes(sha_s, s_fw_expected, sizeof(s_fw_expected))) {
        send_resp(op, id, false, "missing size or sha256");
        return;
    }
    if (ota_get_slot_sizes(&primary, &secondary) != 0 || size > secondary) {
        send_resp(op, id, false, "image larger than slot");
        return;
    }

    if (s_begin_pending) {
        fw_abort();
    }
    if (s_xfer.active) {
        if (s_xfer.type == COMP_XFER_FW_DATA) {
            fw_abort();
        } else {
            akira_free_buffer(s_xfer.buf);
            memset(&s_xfer, 0, sizeof(s_xfer));
            akira_sd_card_set_transfer_active(false);
        }
    }

    if (ota_start_update(size) != OTA_OK) {
        send_resp(op, id, false, "ota start failed");
        return;
    }

    /* The worker erases the whole slot before it drains data (2-30 s).
     * Frames sent earlier would fill the 4 KB pipe and time out, so the
     * response goes out once the worker reports Ready. */
    s_begin_id       = id;
    s_begin_size     = size;
    s_begin_deadline = k_uptime_get() + FW_ERASE_TIMEOUT_MS;
    s_begin_pending  = true;
    k_work_reschedule(&s_begin_work, K_MSEC(FW_ERASE_POLL_MS));
}

static void begin_poll_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!s_begin_pending) {
        return;
    }
    const struct ota_progress *p = ota_get_progress();
    if (p->state == OTA_STATE_ERROR) {
        s_begin_pending = false;
        send_resp(COMP_OP_OTA_BEGIN, s_begin_id, false, "flash erase failed");
        return;
    }
    if (p->state == OTA_STATE_RECEIVING && strcmp(p->status_message, "Ready") == 0) {
        s_begin_pending = false;
        mbedtls_sha256_init(&s_fw_sha);
        mbedtls_sha256_starts(&s_fw_sha, 0);
        s_xfer.type          = COMP_XFER_FW_DATA;
        s_xfer.expected_size = s_begin_size;
        s_xfer.received      = 0;
        s_xfer.active        = true;
        akira_sd_card_set_transfer_active(true);
        LOG_INF("BLE OTA begin: size=%u", s_begin_size);
        send_resp(COMP_OP_OTA_BEGIN, s_begin_id, true, NULL);
        return;
    }
    if (k_uptime_get() > s_begin_deadline) {
        s_begin_pending = false;
        ota_abort_update();
        send_resp(COMP_OP_OTA_BEGIN, s_begin_id, false, "flash erase timeout");
        return;
    }
    k_work_reschedule(&s_begin_work, K_MSEC(FW_ERASE_POLL_MS));
}

static void handle_ota_end(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    if (!s_xfer.active || s_xfer.type != COMP_XFER_FW_DATA) {
        send_resp(op, id, false, "no active transfer");
        return;
    }
    if (s_xfer.received != s_xfer.expected_size) {
        fw_abort();
        send_resp(op, id, false, "incomplete image");
        return;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&s_fw_sha, digest);
    if (memcmp(digest, s_fw_expected, sizeof(digest)) != 0) {
        LOG_ERR("BLE OTA sha256 mismatch");
        fw_abort();
        send_resp(op, id, false, "sha256 mismatch");
        return;
    }

    enum ota_result rc = ota_finalize_update();
    mbedtls_sha256_free(&s_fw_sha);
    memset(&s_xfer, 0, sizeof(s_xfer));
    akira_sd_card_set_transfer_active(false);
    if (rc != OTA_OK) {
        send_resp(op, id, false, ota_result_to_string(rc));
        return;
    }
    send_resp(op, id, true, NULL);
}

static void handle_ota_apply(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    if (ota_get_progress()->state != OTA_STATE_COMPLETE) {
        send_resp(op, id, false, "no staged image");
        return;
    }
    send_resp(op, id, true, NULL);
    ota_reboot_to_apply_update(1000);
}

#else

static void handle_ota_unavailable(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    send_resp(op, id, false, "ota unavailable in this build");
}

#endif

/* The OTA manager is only compiled when CONFIG_AKIRA_OTA is set (it also needs
 * FLASH_MAP + BOOTLOADER_MCUBOOT).  The companion service is useful without it
 * — app install, settings, files and shell passthrough are all independent —
 * so report a well-formed "unavailable" status rather than forcing every
 * companion build to link the whole OTA manager. */
static void handle_ota_status(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    char buf[CHAR_BUF_SIZE];
#ifdef CONFIG_AKIRA_OTA
    const struct ota_progress *st = ota_get_progress();
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"progress\":%u,\"version\":\"\"}",
             st ? ota_state_to_string(st->state) : "idle",
             st ? st->percentage : 0);
#else
    snprintf(buf, sizeof(buf),
             "{\"state\":\"unavailable\",\"progress\":0,\"version\":\"\"}");
#endif
    send_resp(op, id, true, buf);
}

static void handle_fs_session_open(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    if (s_fs_session) {
        k_work_reschedule(&s_fs_lease, K_MSEC(FS_LEASE_MS));
        send_resp(op, id, true, NULL);
        return;
    }
    if (SD_IN_MSC()) {
        send_resp(op, id, false, "sd_in_usb_mode");
        return;
    }
    bool ota_pending = false;
#ifdef CONFIG_AKIRA_OTA
    ota_pending = s_begin_pending;
#endif
    if (s_xfer.active || ota_pending || akira_sd_card_is_transfer_active()) {
        send_resp(op, id, false, "busy");
        return;
    }
    if (!akira_sd_card_is_present()) {
        send_resp(op, id, false, "sd_unavailable");
        return;
    }
    s_fs_session = true;
    akira_sd_card_set_fs_session(true);
    if (SD_IN_MSC()) {                      /* MSC took the card between the check and the flag */
        fs_session_release();
        send_resp(op, id, false, "sd_in_usb_mode");
        return;
    }
    k_work_reschedule(&s_fs_lease, K_MSEC(FS_LEASE_MS));
    send_resp(op, id, true, NULL);
}

static void handle_fs_session_close(const char *op, int id, const char *params)
{
    ARG_UNUSED(params);
    fs_session_release();
    send_resp(op, id, true, NULL);
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

    /* Disable BT IRQ briefly to snapshot the pending command */
    unsigned int key = irq_lock();
    len = s_pending_cmd_len;
    memcpy(buf, s_pending_cmd, len);
    s_pending_cmd_len = 0;
    irq_unlock(key);

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
    else if (strcmp(op, COMP_OP_SETTINGS_GET)        == 0) { handle_settings_get(op, id, params); }
    else if (strcmp(op, COMP_OP_SETTINGS_SET)        == 0) { handle_settings_set(op, id, params); }
    else if (strcmp(op, COMP_OP_SETTINGS_LIST)       == 0) { handle_settings_list(op, id, params); }
    else if (strcmp(op, COMP_OP_SHELL_EXEC)          == 0) { handle_shell_exec(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_LIST)          == 0) { handle_files_list(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_READ)          == 0) { handle_files_read(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_WRITE)         == 0) { handle_files_write(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_DELETE)        == 0) { handle_files_delete(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_MKDIR)         == 0) { handle_files_mkdir(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_WRITE_END)     == 0) { handle_files_write_end(op, id, params); }
    else if (strcmp(op, COMP_OP_FILES_RENAME)        == 0) { handle_files_rename(op, id, params); }
    else if (strcmp(op, COMP_OP_FS_SESSION_OPEN)     == 0) { handle_fs_session_open(op, id, params); }
    else if (strcmp(op, COMP_OP_FS_SESSION_CLOSE)    == 0) { handle_fs_session_close(op, id, params); }
#ifdef CONFIG_AKIRA_OTA
    else if (strcmp(op, COMP_OP_OTA_BEGIN)           == 0) { handle_ota_begin(op, id, params); }
    else if (strcmp(op, COMP_OP_OTA_END)             == 0) { handle_ota_end(op, id, params); }
    else if (strcmp(op, COMP_OP_OTA_APPLY)           == 0) { handle_ota_apply(op, id, params); }
#else
    else if (strcmp(op, COMP_OP_OTA_BEGIN)           == 0 ||
             strcmp(op, COMP_OP_OTA_END)             == 0 ||
             strcmp(op, COMP_OP_OTA_APPLY)           == 0) { handle_ota_unavailable(op, id, params); }
#endif
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

    unsigned int irq_key = irq_lock();
    memcpy(s_pending_cmd, buf, len);
    s_pending_cmd_len = len;
    irq_unlock(irq_key);

    k_work_submit(&s_cmd_work);
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

#ifdef CONFIG_AKIRA_OTA
    if (s_xfer.type == COMP_XFER_FW_DATA) {
        if ((xflags & COMP_FLAG_ERROR) ||
            s_xfer.received + plen > s_xfer.expected_size ||
            (plen && ota_write_chunk(frame + 4, plen) != OTA_OK)) {
            LOG_ERR("BLE OTA aborted at %u/%u", s_xfer.received, s_xfer.expected_size);
            fw_abort();
            return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
        }
        mbedtls_sha256_update(&s_fw_sha, frame + 4, plen);
        s_xfer.received += plen;
        return (ssize_t)len;
    }
#endif

    /* Copy payload into staging buffer */
    if (s_xfer.received + plen > s_xfer.capacity) {
        LOG_ERR("Transfer overflow: received %u + %u > capacity %u",
                s_xfer.received, plen, s_xfer.capacity);
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
        akira_sd_card_set_transfer_active(false);
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }

    memcpy(s_xfer.buf + s_xfer.received, frame + 4, plen);
    s_xfer.received += plen;

    if (xflags & COMP_FLAG_ERROR) {
        LOG_WRN("Transfer aborted by peer");
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
        akira_sd_card_set_transfer_active(false);
        return (ssize_t)len;
    }

    if (xflags & COMP_FLAG_LAST) {
        /* Staged. COMP_XFER_APP_DATA is consumed by apps.install.end and
         * COMP_XFER_FILE_DATA by files.write.end. Both must SURVIVE until then:
         * freeing here left the end handler seeing s_xfer.active == false, so it
         * answered "no active transfer" and silently did nothing (no device-side
         * log, only the phone saw the error). Cleanup happens in the end handler,
         * and on disconnect / the next begin if the peer goes away. */
        LOG_INF("BLE transfer staged: %u bytes", s_xfer.received);
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
    LOG_DBG("RESP_CHAR CCC: %s", value == BT_GATT_CCC_NOTIFY ? "notify" : "off");
}

static void data_dn_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    LOG_DBG("DATA_DOWN CCC: %s", value == BT_GATT_CCC_NOTIFY ? "notify" : "off");
}

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    LOG_DBG("STATUS CCC: %s", value == BT_GATT_CCC_NOTIFY ? "notify" : "off");
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

/* Every attribute requires an encrypted link, so the first access makes the phone
 * pair and bond (Just Works). Unbonded peers cannot reach any companion op. */
BT_GATT_SERVICE_DEFINE(companion_svc_def,
    BT_GATT_PRIMARY_SERVICE(&svc_uuid),

    /* CMD_CHAR: WRITE */
    BT_GATT_CHARACTERISTIC(&cmd_uuid.uuid,
                            BT_GATT_CHRC_WRITE,
                            BT_GATT_PERM_WRITE_ENCRYPT,
                            NULL, cmd_write, s_cmd_buf),

    /* RESP_CHAR: NOTIFY */
    BT_GATT_CHARACTERISTIC(&resp_uuid.uuid,
                            BT_GATT_CHRC_NOTIFY,
                            BT_GATT_PERM_NONE,
                            NULL, NULL, s_resp_buf),
    BT_GATT_CCC(resp_ccc_changed,
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

    /* DATA_UP: WRITE_WITHOUT_RSP */
    BT_GATT_CHARACTERISTIC(&data_up_uuid.uuid,
                            BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                            BT_GATT_PERM_WRITE_ENCRYPT,
                            NULL, data_up_write, NULL),

    /* DATA_DOWN: NOTIFY */
    BT_GATT_CHARACTERISTIC(&data_dn_uuid.uuid,
                            BT_GATT_CHRC_NOTIFY,
                            BT_GATT_PERM_NONE,
                            NULL, NULL, s_data_dn_buf),
    BT_GATT_CCC(data_dn_ccc_changed,
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

    /* STATUS: READ + NOTIFY */
    BT_GATT_CHARACTERISTIC(&status_uuid.uuid,
                            BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                            BT_GATT_PERM_READ_ENCRYPT,
                            status_read, NULL, s_status_buf),
    BT_GATT_CCC(status_ccc_changed,
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
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

    /* The negotiated ATT MTU bounds every DATA_UP frame the phone can send.
     * A peer chunking to CHAR_BUF_SIZE (244 B) needs an MTU of at least 247;
     * anything lower and its writes are rejected client-side, which surfaces
     * only as an opaque "GATT operation failed" with nothing logged here.
     * MTU exchange happens shortly after connect, so this is logged from the
     * exchange callback below rather than here. */
    companion_svc_notify_status();
}

/* ATT MTU changes are reported through bt_gatt_cb, not bt_conn_cb. */
static void conn_cb_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
    ARG_UNUSED(conn);
    uint16_t usable = (rx < tx ? rx : tx);

    if (usable < CHAR_BUF_SIZE + 3U) {
        LOG_WRN("Companion: ATT MTU %u (tx=%u rx=%u) is below the %u needed "
                "for %u-byte transfers — peer writes will fail",
                usable, tx, rx, CHAR_BUF_SIZE + 3U, CHAR_BUF_SIZE);
    } else {
        LOG_INF("Companion: ATT MTU %u (tx=%u rx=%u)", usable, tx, rx);
    }
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
#ifdef CONFIG_AKIRA_OTA
    fw_abort();
#endif
    if (s_xfer.active) {
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
        akira_sd_card_set_transfer_active(false);
    }
    fs_session_release();
}

BT_CONN_CB_DEFINE(companion_conn_cb) = {
    .connected    = conn_cb_connected,
    .disconnected = conn_cb_disconnected,
};

static struct bt_gatt_cb companion_gatt_cb = {
    .att_mtu_updated = conn_cb_mtu_updated,
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

    k_work_init(&s_cmd_work, cmd_work_handler);
    k_work_init_delayable(&s_status_timer, status_timer_handler);
    k_work_init_delayable(&s_fs_lease, fs_lease_expired);
#ifdef CONFIG_AKIRA_OTA
    k_work_init_delayable(&s_begin_work, begin_poll_handler);
#endif

    bt_gatt_cb_register(&companion_gatt_cb);

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

#ifdef CONFIG_AKIRA_OTA
    fw_abort();
#endif
    if (s_xfer.active) {
        akira_free_buffer(s_xfer.buf);
        memset(&s_xfer, 0, sizeof(s_xfer));
        akira_sd_card_set_transfer_active(false);
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

    /* Build the same JSON as cloud device.status. free_heap is reported as 0:
     * there is no portable free-heap query here and passing NULL to a slab
     * accessor would fault. */
    int n = snprintf((char *)s_status_buf, sizeof(s_status_buf),
                     "{\"fw\":\"%s\",\"free_heap\":0,"
                     "\"running_apps\":[],\"bt_rssi\":0}",
                     AKIRA_VERSION_STRING);

    if (n <= 0 || (size_t)n >= sizeof(s_status_buf)) {
        return;
    }

    int rc = bt_gatt_notify(s_conn, &companion_svc_def.attrs[12], /* status val */
                            s_status_buf, (uint16_t)n);
    if (rc && rc != -ENOTCONN) {
        LOG_DBG("status notify failed: %d", rc);
    }
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
    /* HID and Companion are mutually exclusive on the single BLE connection.
     * Only claim the radio when the persisted boot mode selects companion;
     * otherwise stay dormant so the HID profile keeps working. Switch modes
     * with the `btmode companion` shell command (persists + reboots). */
    if (!bt_manager_boot_mode_is_companion()) {
        LOG_INF("Companion: boot mode != companion; service dormant");
        return 0;
    }
    return companion_svc_init();
}
SYS_INIT(companion_auto_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif
