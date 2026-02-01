/**
 * @file bt_app_transfer.c
 * @brief BLE WASM App Transfer Service Implementation
 */

#include "bt_app_transfer.h"
#include <runtime/app_loader/app_loader.h>
#include <runtime/app_manager/app_manager.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/fs/fs.h>
#include <string.h>

LOG_MODULE_REGISTER(bt_app_xfer, CONFIG_AKIRA_LOG_LEVEL);

/* Custom UUIDs for App Transfer Service */
#define BT_UUID_APP_XFER_VAL \
    BT_UUID_128_ENCODE(0x414B4952, 0x0001, 0x0001, 0x0001, 0x000000000001)
#define BT_UUID_APP_XFER BT_UUID_DECLARE_128(BT_UUID_APP_XFER_VAL)

#define BT_UUID_APP_RX_DATA_VAL \
    BT_UUID_128_ENCODE(0x414B4952, 0x0001, 0x0001, 0x0001, 0x000000000002)
#define BT_UUID_APP_RX_DATA BT_UUID_DECLARE_128(BT_UUID_APP_RX_DATA_VAL)

#define BT_UUID_APP_TX_STATUS_VAL \
    BT_UUID_128_ENCODE(0x414B4952, 0x0001, 0x0001, 0x0001, 0x000000000003)
#define BT_UUID_APP_TX_STATUS BT_UUID_DECLARE_128(BT_UUID_APP_TX_STATUS_VAL)

#define BT_UUID_APP_CONTROL_VAL \
    BT_UUID_128_ENCODE(0x414B4952, 0x0001, 0x0001, 0x0001, 0x000000000004)
#define BT_UUID_APP_CONTROL BT_UUID_DECLARE_128(BT_UUID_APP_CONTROL_VAL)

/* Maximum app size for BLE transfer (in bytes) */
#ifndef CONFIG_AKIRA_APP_MAX_SIZE_KB
#define CONFIG_AKIRA_APP_MAX_SIZE_KB 64
#endif
#define MAX_APP_SIZE (CONFIG_AKIRA_APP_MAX_SIZE_KB * 1024)

/* Temp file for receiving app */
#define TEMP_APP_PATH "/lfs/apps/.tmp_xfer.wasm"

/* Transfer context */
static struct
{
    bt_app_xfer_state_t state;
    char app_name[32];
    uint32_t total_size;
    uint32_t received_bytes;
    uint32_t expected_crc;
    uint32_t running_crc;
    struct fs_file_t file;
    bool file_open;
    bt_app_xfer_complete_cb_t callback;
} g_xfer = {
    .state = BT_APP_XFER_IDLE,
    .file_open = false};

static K_MUTEX_DEFINE(xfer_mutex);

/* Forward declarations */
static ssize_t rx_data_write(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags);
static ssize_t control_write(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags);
static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* Status notification value */
static uint8_t status_value[4] = {0};
static bool notify_enabled = false;

/* GATT Service Definition */
BT_GATT_SERVICE_DEFINE(app_xfer_svc,
                       BT_GATT_PRIMARY_SERVICE(BT_UUID_APP_XFER),

                       /* RX_DATA: Write without response for app chunks */
                       BT_GATT_CHARACTERISTIC(BT_UUID_APP_RX_DATA,
                                              BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                              BT_GATT_PERM_WRITE,
                                              NULL, rx_data_write, NULL),

                       /* TX_STATUS: Notify for status updates */
                       BT_GATT_CHARACTERISTIC(BT_UUID_APP_TX_STATUS,
                                              BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_NONE,
                                              NULL, NULL, status_value),
                       BT_GATT_CCC(status_ccc_changed,
                                   BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

                       /* CONTROL: Write for transfer control commands */
                       BT_GATT_CHARACTERISTIC(BT_UUID_APP_CONTROL,
                                              BT_GATT_CHRC_WRITE,
                                              BT_GATT_PERM_WRITE,
                                              NULL, control_write, NULL), );

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_DBG("Status notifications %s", notify_enabled ? "enabled" : "disabled");
}

static void send_status(bt_app_status_t status, uint8_t progress)
{
    if (!notify_enabled)
    {
        return;
    }

    status_value[0] = g_xfer.state;
    status_value[1] = status;
    status_value[2] = progress;
    status_value[3] = 0;

    bt_gatt_notify(NULL, &app_xfer_svc.attrs[4], status_value, sizeof(status_value));
}

static void cleanup_transfer(void)
{
    if (g_xfer.file_open)
    {
        fs_close(&g_xfer.file);
        g_xfer.file_open = false;
    }
    fs_unlink(TEMP_APP_PATH);
    memset(g_xfer.app_name, 0, sizeof(g_xfer.app_name));
    g_xfer.total_size = 0;
    g_xfer.received_bytes = 0;
    g_xfer.expected_crc = 0;
    g_xfer.running_crc = 0;
}

static int start_transfer(const struct bt_app_xfer_header *header)
{
    if (g_xfer.state != BT_APP_XFER_IDLE)
    {
        LOG_WRN("Transfer already in progress");
        return -EBUSY;
    }

    if (header->total_size > MAX_APP_SIZE)
    {
        LOG_ERR("App too large: %u > %u", header->total_size, MAX_APP_SIZE);
        return -ENOMEM;
    }

    /* Initialize transfer context */
    strncpy(g_xfer.app_name, header->name, sizeof(g_xfer.app_name) - 1);
    g_xfer.total_size = header->total_size;
    g_xfer.expected_crc = header->expected_crc;
    g_xfer.received_bytes = 0;
    g_xfer.running_crc = 0;

    /* Open temp file */
    fs_file_t_init(&g_xfer.file);
    int ret = fs_open(&g_xfer.file, TEMP_APP_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0)
    {
        LOG_ERR("Failed to create temp file: %d", ret);
        return ret;
    }
    g_xfer.file_open = true;

    g_xfer.state = BT_APP_XFER_RECEIVING;
    LOG_INF("Starting transfer: %s (%u bytes)", g_xfer.app_name, g_xfer.total_size);

    send_status(BT_APP_STATUS_OK, 0);
    return 0;
}

static int finalize_transfer(void)
{
    if (g_xfer.state != BT_APP_XFER_RECEIVING)
    {
        return -EINVAL;
    }

    g_xfer.state = BT_APP_XFER_VALIDATING;

    /* Close file */
    if (g_xfer.file_open)
    {
        fs_close(&g_xfer.file);
        g_xfer.file_open = false;
    }

    /* Verify size */
    if (g_xfer.received_bytes != g_xfer.total_size)
    {
        LOG_ERR("Size mismatch: %u != %u", g_xfer.received_bytes, g_xfer.total_size);
        g_xfer.state = BT_APP_XFER_ERROR;
        send_status(BT_APP_STATUS_SIZE_ERROR, 100);
        cleanup_transfer();
        return -EINVAL;
    }

    /* Verify CRC */
    if (g_xfer.running_crc != g_xfer.expected_crc)
    {
        LOG_ERR("CRC mismatch: 0x%08X != 0x%08X", g_xfer.running_crc, g_xfer.expected_crc);
        g_xfer.state = BT_APP_XFER_ERROR;
        send_status(BT_APP_STATUS_CRC_FAIL, 100);
        cleanup_transfer();
        return -EINVAL;
    }

    /* Install app */
    g_xfer.state = BT_APP_XFER_INSTALLING;
    LOG_INF("Installing app: %s", g_xfer.app_name);

    int ret = app_manager_install_from_path(TEMP_APP_PATH);
    if (ret < 0)
    {
        LOG_ERR("Install failed: %d", ret);
        g_xfer.state = BT_APP_XFER_ERROR;
        send_status(BT_APP_STATUS_INSTALL_FAIL, 100);
        if (g_xfer.callback)
        {
            g_xfer.callback(false, g_xfer.app_name, ret);
        }
        cleanup_transfer();
        return ret;
    }

    /* Success */
    g_xfer.state = BT_APP_XFER_COMPLETE;
    LOG_INF("App installed successfully: %s", g_xfer.app_name);
    send_status(BT_APP_STATUS_OK, 100);

    if (g_xfer.callback)
    {
        g_xfer.callback(true, g_xfer.app_name, 0);
    }

    /* Cleanup temp file */
    fs_unlink(TEMP_APP_PATH);

    /* Reset to idle */
    g_xfer.state = BT_APP_XFER_IDLE;
    memset(g_xfer.app_name, 0, sizeof(g_xfer.app_name));

    return 0;
}

static ssize_t rx_data_write(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (g_xfer.state != BT_APP_XFER_RECEIVING)
    {
        LOG_WRN("Not in receiving state");
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }

    k_mutex_lock(&xfer_mutex, K_FOREVER);

    /* Check for overflow */
    if (g_xfer.received_bytes + len > g_xfer.total_size)
    {
        LOG_ERR("Received more data than expected");
        k_mutex_unlock(&xfer_mutex);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    /* Write to file */
    ssize_t written = fs_write(&g_xfer.file, buf, len);
    if (written != len)
    {
        LOG_ERR("Write failed: %d", (int)written);
        k_mutex_unlock(&xfer_mutex);
        g_xfer.state = BT_APP_XFER_ERROR;
        send_status(BT_APP_STATUS_ERROR, 0);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    /* Update CRC */
    g_xfer.running_crc = crc32_ieee_update(g_xfer.running_crc, buf, len);
    g_xfer.received_bytes += len;

    /* Send progress update */
    uint8_t progress = (g_xfer.received_bytes * 100) / g_xfer.total_size;
    send_status(BT_APP_STATUS_OK, progress);

    k_mutex_unlock(&xfer_mutex);
    return len;
}

static ssize_t control_write(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (len < 1)
    {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const uint8_t *data = buf;
    bt_app_cmd_t cmd = data[0];

    k_mutex_lock(&xfer_mutex, K_FOREVER);

    switch (cmd)
    {
    case BT_APP_CMD_START:
        if (len < 1 + sizeof(struct bt_app_xfer_header))
        {
            k_mutex_unlock(&xfer_mutex);
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        start_transfer((const struct bt_app_xfer_header *)&data[1]);
        break;

    case BT_APP_CMD_ABORT:
        LOG_INF("Transfer aborted by client");
        cleanup_transfer();
        g_xfer.state = BT_APP_XFER_IDLE;
        send_status(BT_APP_STATUS_OK, 0);
        break;

    case BT_APP_CMD_COMMIT:
        finalize_transfer();
        break;

    case BT_APP_CMD_STATUS:
    {
        uint8_t progress = 0;
        if (g_xfer.total_size > 0)
        {
            progress = (g_xfer.received_bytes * 100) / g_xfer.total_size;
        }
        send_status(BT_APP_STATUS_OK, progress);
    }
    break;

    default:
        LOG_WRN("Unknown command: 0x%02X", cmd);
        k_mutex_unlock(&xfer_mutex);
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    }

    k_mutex_unlock(&xfer_mutex);
    return len;
}

/* Public API */

int bt_app_transfer_init(void)
{
    LOG_INF("BLE App Transfer Service initialized");
    return 0;
}

bt_app_xfer_state_t bt_app_transfer_get_state(void)
{
    return g_xfer.state;
}

int bt_app_transfer_get_progress(struct bt_app_xfer_progress *progress)
{
    if (!progress)
    {
        return -EINVAL;
    }

    k_mutex_lock(&xfer_mutex, K_FOREVER);
    progress->state = g_xfer.state;
    strncpy(progress->app_name, g_xfer.app_name, sizeof(progress->app_name) - 1);
    progress->total_size = g_xfer.total_size;
    progress->received_bytes = g_xfer.received_bytes;
    if (g_xfer.total_size > 0)
    {
        progress->percent_complete = (g_xfer.received_bytes * 100) / g_xfer.total_size;
    }
    else
    {
        progress->percent_complete = 0;
    }
    k_mutex_unlock(&xfer_mutex);

    return 0;
}

void bt_app_transfer_abort(void)
{
    k_mutex_lock(&xfer_mutex, K_FOREVER);
    if (g_xfer.state == BT_APP_XFER_RECEIVING)
    {
        cleanup_transfer();
        g_xfer.state = BT_APP_XFER_IDLE;
        LOG_INF("Transfer aborted");
    }
    k_mutex_unlock(&xfer_mutex);
}

bool bt_app_transfer_is_ready(void)
{
    return g_xfer.state == BT_APP_XFER_IDLE;
}

void bt_app_transfer_set_callback(bt_app_xfer_complete_cb_t callback)
{
    g_xfer.callback = callback;
}
