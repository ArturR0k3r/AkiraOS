/**
 * @file hid_app_handler.c
 * @brief AkiraOS USB HID Raw Application Command Handler
 *
 * Dispatches AkiraOS WebHID commands arriving on USB HID Report ID 3
 * to the app manager and system APIs.  Responses are sent back via
 * usb_hid_raw_send().
 *
 * The raw OUT report callback is called from the USB interrupt context,
 * so actual work is deferred to a dedicated kernel work item.
 */

#include "hid_app_handler.h"
#include "usb/usb_hid.h"
#include "akira.h"
#include "drivers/platform_hal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef CONFIG_AKIRA_APP_MANAGER
#include "runtime/app_manager/app_manager.h"
#endif

LOG_MODULE_REGISTER(hid_app_handler, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Packet helpers                                                            */
/*===========================================================================*/

/**
 * @brief Build a 63-byte response payload in @p out.
 *
 * @param cmd     Original command byte (high bit will be set for response)
 * @param seq     Sequence number echoed from request
 * @param status  0 = OK, non-zero = error
 * @param flags   HID_FLAG_MORE_DATA if there are more response packets
 * @param payload Response data bytes (may be NULL if len == 0)
 * @param len     Number of bytes to copy from @p payload (max HID_PKT_PAYLOAD_SIZE)
 */
static void build_response(uint8_t *out, uint8_t cmd, uint8_t seq,
                           uint8_t status, uint8_t flags,
                           const void *payload, uint16_t len)
{
    memset(out, 0, USB_HID_RAW_PAYLOAD_SIZE);
    out[HID_PKT_CMD] = cmd | 0x80u;
    out[HID_PKT_SEQ] = seq;
    out[HID_PKT_FLAGS] = flags | (status << 4u); /* upper nibble = status */
    out[HID_PKT_LEN_LO] = (uint8_t)(len & 0xFF);
    out[HID_PKT_LEN_HI] = (uint8_t)(len >> 8);
    if (payload && len > 0)
    {
        uint16_t copy = MIN(len, (uint16_t)HID_PKT_PAYLOAD_SIZE);
        memcpy(&out[HID_PKT_PAYLOAD], payload, copy);
    }
}

static void send_response(uint8_t cmd, uint8_t seq, uint8_t status,
                          const void *payload, uint16_t len)
{
    uint8_t pkt[USB_HID_RAW_PAYLOAD_SIZE];
    build_response(pkt, cmd, seq, status, 0, payload, len);
    usb_hid_raw_send(pkt);
}

static void send_error(uint8_t cmd, uint8_t seq, uint8_t err)
{
    send_response(cmd, seq, err, NULL, 0);
}

/*===========================================================================*/
/* Deferred work                                                             */
/*===========================================================================*/

#define CMD_BUF_SIZE USB_HID_RAW_PAYLOAD_SIZE

static struct
{
    struct k_work work;
    uint8_t buf[CMD_BUF_SIZE];
    uint8_t len;
} cmd_work;

static K_MUTEX_DEFINE(install_mutex);

/* Chunked install session state */
static struct
{
    int session; /* -1 = no active session */
    uint16_t chunks;
} install_ctx = {.session = -1};

/*===========================================================================*/
/* Command handlers                                                          */
/*===========================================================================*/

static void handle_get_info(uint8_t seq)
{
    char payload[HID_PKT_PAYLOAD_SIZE];
    int n = snprintf(payload, sizeof(payload),
                     AKIRA_VERSION_STRING "|%s",
                     akira_get_platform_name());
    send_response(HID_CMD_GET_INFO, seq, 0, payload, (uint16_t)MIN(n, (int)sizeof(payload)));
}

static void handle_get_status(uint8_t seq)
{
    uint32_t free_kb = 0, total_kb = 0;
#ifdef CONFIG_FILE_SYSTEM
    struct fs_statvfs stat;
    if (fs_statvfs("/lfs", &stat) == 0)
    {
        free_kb = (uint32_t)((uint64_t)stat.f_bfree * stat.f_frsize / 1024);
        total_kb = (uint32_t)((uint64_t)stat.f_blocks * stat.f_frsize / 1024);
    }
#endif
    uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000U);

    /* Compact binary: [free_kb:u32][total_kb:u32][uptime_s:u32] = 12 bytes */
    uint8_t payload[12];
    memcpy(&payload[0], &free_kb, 4);
    memcpy(&payload[4], &total_kb, 4);
    memcpy(&payload[8], &uptime_s, 4);
    send_response(HID_CMD_GET_STATUS, seq, 0, payload, 12);
}

static void handle_get_apps(uint8_t seq)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    int count;
    app_info_t *apps = app_manager_list_alloc(&count);
    if (!apps)
        count = 0;

    /* First packet: [count:u8] then app entries packed as [id:u8][state:u8][name:24] */
    for (int i = 0; i < count; i++)
    {
        uint8_t entry[HID_PKT_PAYLOAD_SIZE];
        memset(entry, 0, sizeof(entry));
        entry[0] = apps[i].id;
        entry[1] = (uint8_t)apps[i].state;
        strncpy((char *)&entry[2], apps[i].name, 24);
        strncpy((char *)&entry[26], apps[i].version, 8);
        entry[34] = (count > 0) ? (uint8_t)count : 1;
        entry[35] = (uint8_t)i; /* index */
        bool more = (i < count - 1);
        uint8_t pkt[USB_HID_RAW_PAYLOAD_SIZE];
        build_response(pkt, HID_CMD_GET_APPS, seq,
                       0, more ? HID_FLAG_MORE_DATA : 0,
                       entry, sizeof(entry));
        usb_hid_raw_send(pkt);
    }
    if (count == 0)
    {
        send_response(HID_CMD_GET_APPS, seq, 0, NULL, 0);
    }
    akira_free_buffer(apps);
#else
    send_error(HID_CMD_GET_APPS, seq, 0x01);
#endif
}

static void handle_get_logs(uint8_t seq)
{
    /* Stub: no log ring buffer yet */
    const char *msg = "no logs";
    send_response(HID_CMD_GET_LOGS, seq, 0, msg, (uint16_t)strlen(msg));
}

static void handle_install_begin(uint8_t seq, const uint8_t *payload, uint8_t plen)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    k_mutex_lock(&install_mutex, K_FOREVER);
    if (install_ctx.session >= 0)
    {
        k_mutex_unlock(&install_mutex);
        send_error(HID_CMD_INSTALL_BEGIN, seq, 0x02); /* already in progress */
        return;
    }

    /* Payload: [total_size:u32][name:null-terminated (max 24)] */
    if (plen < 5)
    {
        k_mutex_unlock(&install_mutex);
        send_error(HID_CMD_INSTALL_BEGIN, seq, 0x03); /* bad payload */
        return;
    }
    uint32_t total_size;
    memcpy(&total_size, payload, 4);
    char name[APP_NAME_MAX_LEN] = "hid_app";
    strncpy(name, (const char *)&payload[4],
            MIN((int)(plen - 4), APP_NAME_MAX_LEN - 1));

    int session = app_manager_install_begin(name, total_size, APP_SOURCE_USB);
    if (session < 0)
    {
        k_mutex_unlock(&install_mutex);
        send_error(HID_CMD_INSTALL_BEGIN, seq, 0x04);
        return;
    }
    install_ctx.session = session;
    install_ctx.chunks = 0;
    k_mutex_unlock(&install_mutex);
    send_response(HID_CMD_INSTALL_BEGIN, seq, 0, NULL, 0);
#else
    send_error(HID_CMD_INSTALL_BEGIN, seq, 0x01);
#endif
}

static void handle_install_chunk(uint8_t seq, const uint8_t *payload, uint8_t plen)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    k_mutex_lock(&install_mutex, K_FOREVER);
    if (install_ctx.session < 0)
    {
        k_mutex_unlock(&install_mutex);
        send_error(HID_CMD_INSTALL_CHUNK, seq, 0x02); /* no session */
        return;
    }
    int ret = app_manager_install_chunk(install_ctx.session, payload, plen);
    install_ctx.chunks++;
    k_mutex_unlock(&install_mutex);
    if (ret < 0)
    {
        send_error(HID_CMD_INSTALL_CHUNK, seq, 0x05);
    }
    else
    {
        send_response(HID_CMD_INSTALL_CHUNK, seq, 0, NULL, 0);
    }
#else
    send_error(HID_CMD_INSTALL_CHUNK, seq, 0x01);
#endif
}

static void handle_install_end(uint8_t seq)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    k_mutex_lock(&install_mutex, K_FOREVER);
    if (install_ctx.session < 0)
    {
        k_mutex_unlock(&install_mutex);
        send_error(HID_CMD_INSTALL_END, seq, 0x02);
        return;
    }
    int app_id = app_manager_install_end(install_ctx.session, NULL);
    install_ctx.session = -1;
    k_mutex_unlock(&install_mutex);
    if (app_id < 0)
    {
        send_error(HID_CMD_INSTALL_END, seq, 0x06);
    }
    else
    {
        uint8_t resp[2] = {(uint8_t)(app_id & 0xFF), 0};
        send_response(HID_CMD_INSTALL_END, seq, 0, resp, 2);
    }
#else
    send_error(HID_CMD_INSTALL_END, seq, 0x01);
#endif
}

static void handle_install_abort(uint8_t seq)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    k_mutex_lock(&install_mutex, K_FOREVER);
    if (install_ctx.session >= 0)
    {
        app_manager_install_abort(install_ctx.session);
        install_ctx.session = -1;
    }
    k_mutex_unlock(&install_mutex);
#endif
    send_response(HID_CMD_INSTALL_ABORT, seq, 0, NULL, 0);
}

static void handle_app_start(uint8_t seq, const uint8_t *payload, uint8_t plen)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    char name[APP_NAME_MAX_LEN] = {0};
    strncpy(name, (const char *)payload, MIN((int)plen, APP_NAME_MAX_LEN - 1));
    int ret = app_manager_start(name);
    send_response(HID_CMD_APP_START, seq, ret < 0 ? 0x01 : 0, NULL, 0);
#else
    send_error(HID_CMD_APP_START, seq, 0x01);
#endif
}

static void handle_app_stop(uint8_t seq, const uint8_t *payload, uint8_t plen)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    char name[APP_NAME_MAX_LEN] = {0};
    strncpy(name, (const char *)payload, MIN((int)plen, APP_NAME_MAX_LEN - 1));
    int ret = app_manager_stop(name);
    send_response(HID_CMD_APP_STOP, seq, ret < 0 ? 0x01 : 0, NULL, 0);
#else
    send_error(HID_CMD_APP_STOP, seq, 0x01);
#endif
}

static void handle_app_delete(uint8_t seq, const uint8_t *payload, uint8_t plen)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    char name[APP_NAME_MAX_LEN] = {0};
    strncpy(name, (const char *)payload, MIN((int)plen, APP_NAME_MAX_LEN - 1));
    int ret = app_manager_uninstall(name);
    send_response(HID_CMD_APP_DELETE, seq, ret < 0 ? 0x01 : 0, NULL, 0);
#else
    send_error(HID_CMD_APP_DELETE, seq, 0x01);
#endif
}

/*===========================================================================*/
/* Work handler (executes on system work queue, not in ISR context)         */
/*===========================================================================*/

static void cmd_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    uint8_t cmd = cmd_work.buf[HID_PKT_CMD];
    uint8_t seq = cmd_work.buf[HID_PKT_SEQ];
    uint16_t dlen = (uint16_t)cmd_work.buf[HID_PKT_LEN_LO] |
                    ((uint16_t)cmd_work.buf[HID_PKT_LEN_HI] << 8);
    const uint8_t *payload = &cmd_work.buf[HID_PKT_PAYLOAD];
    uint8_t plen = (uint8_t)MIN(dlen, (uint16_t)HID_PKT_PAYLOAD_SIZE);

    LOG_DBG("HID cmd 0x%02X seq=%u plen=%u", cmd, seq, plen);

    switch (cmd)
    {
    case HID_CMD_GET_INFO:
        handle_get_info(seq);
        break;
    case HID_CMD_GET_APPS:
        handle_get_apps(seq);
        break;
    case HID_CMD_GET_STATUS:
        handle_get_status(seq);
        break;
    case HID_CMD_GET_LOGS:
        handle_get_logs(seq);
        break;
    case HID_CMD_INSTALL_BEGIN:
        handle_install_begin(seq, payload, plen);
        break;
    case HID_CMD_INSTALL_CHUNK:
        handle_install_chunk(seq, payload, plen);
        break;
    case HID_CMD_INSTALL_END:
        handle_install_end(seq);
        break;
    case HID_CMD_INSTALL_ABORT:
        handle_install_abort(seq);
        break;
    case HID_CMD_APP_START:
        handle_app_start(seq, payload, plen);
        break;
    case HID_CMD_APP_STOP:
        handle_app_stop(seq, payload, plen);
        break;
    case HID_CMD_APP_DELETE:
        handle_app_delete(seq, payload, plen);
        break;
    default:
        LOG_WRN("Unknown HID cmd 0x%02X", cmd);
        send_error(cmd, seq, 0xFF);
        break;
    }
}

/*===========================================================================*/
/* Raw OUT report callback (ISR context)                                    */
/*===========================================================================*/

static void raw_report_cb(const uint8_t *data, uint8_t len)
{
    if (k_work_is_pending(&cmd_work.work))
    {
        /* Drop: previous command not yet processed */
        return;
    }
    uint8_t copy = MIN(len, (uint8_t)CMD_BUF_SIZE);
    memcpy(cmd_work.buf, data, copy);
    cmd_work.len = copy;
    k_work_submit(&cmd_work.work);
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int hid_app_handler_init(void)
{
    k_work_init(&cmd_work.work, cmd_work_handler);
    usb_hid_raw_set_handler(raw_report_cb);
    LOG_INF("HID app handler initialized (Report ID 3)");
    return 0;
}
