/**
 * @file bt_shell.c
 * @brief Bluetooth Shell Service Implementation
 *
 * GATT service for bidirectional shell communication between
 * AkiraOS device and connected BLE phone/tablet.
 *
 * TX (Device -> Phone): Notifications on Shell TX characteristic
 * RX (Phone -> Device): Writes to Shell RX characteristic
 */

#include "bt_shell.h"
#include "bt_manager.h"
#include <zephyr/logging/log.h>
#include <string.h>

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/bluetooth.h>
#endif

LOG_MODULE_REGISTER(bt_shell, CONFIG_AKIRA_LOG_LEVEL);

#if defined(CONFIG_BT)

/*===========================================================================*/
/* Service UUIDs                                                             */
/*===========================================================================*/

/* Shell Service UUID: d5b1b7e2-7f5a-4eef-8fd0-1a2b3c4d5e71 */
static struct bt_uuid_128 shell_service_uuid = BT_UUID_INIT_128(
    0x71, 0x5e, 0x4d, 0x3c, 0x2b, 0x1a, 0xd0, 0x8f,
    0xef, 0x4e, 0x5a, 0x7f, 0xe2, 0xb7, 0xb1, 0xd5);

/* Shell TX Characteristic UUID: d5b1b7e3-7f5a-4eef-8fd0-1a2b3c4d5e72 */
static struct bt_uuid_128 shell_tx_char_uuid = BT_UUID_INIT_128(
    0x72, 0x5e, 0x4d, 0x3c, 0x2b, 0x1a, 0xd0, 0x8f,
    0xef, 0x4e, 0x5a, 0x7f, 0xe3, 0xb7, 0xb1, 0xd5);

/* Shell RX Characteristic UUID: d5b1b7e4-7f5a-4eef-8fd0-1a2b3c4d5e73 */
static struct bt_uuid_128 shell_rx_char_uuid = BT_UUID_INIT_128(
    0x73, 0x5e, 0x4d, 0x3c, 0x2b, 0x1a, 0xd0, 0x8f,
    0xef, 0x4e, 0x5a, 0x7f, 0xe4, 0xb7, 0xb1, 0xd5);

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    bool notifications_enabled;
    bt_shell_rx_callback_t rx_callback;
} shell_state;

/*===========================================================================*/
/* GATT Callbacks                                                            */
/*===========================================================================*/

/**
 * @brief CCC changed callback - tracks notification subscription state
 */
static void shell_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    shell_state.notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Shell TX notifications %s",
            shell_state.notifications_enabled ? "enabled" : "disabled");
}

/**
 * @brief Write callback for RX characteristic (phone -> device)
 */
static ssize_t shell_rx_write(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len,
                              uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (len == 0)
    {
        return 0;
    }

    const uint8_t *data = buf;

    /* Log received data */
    LOG_INF("Shell RX received (%d bytes)", len);

    /* Call registered callback if any */
    if (shell_state.rx_callback)
    {
        shell_state.rx_callback(data, len);
    }

    return len;
}

/*===========================================================================*/
/* GATT Service Definition                                                   */
/*===========================================================================*/

/* Attribute indices in shell_svc.attrs[]:
 * [0] = Primary Service declaration
 * [1] = TX Characteristic declaration
 * [2] = TX Characteristic value (used for notifications)
 * [3] = TX CCC descriptor
 * [4] = RX Characteristic declaration
 * [5] = RX Characteristic value
 */
BT_GATT_SERVICE_DEFINE(shell_svc,
                       /* Primary Service */
                       BT_GATT_PRIMARY_SERVICE(&shell_service_uuid.uuid),

                       /* TX Characteristic (Device -> Phone, Notify) */
                       BT_GATT_CHARACTERISTIC(&shell_tx_char_uuid.uuid,
                                              BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_NONE,
                                              NULL, NULL, NULL),
                       BT_GATT_CCC(shell_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

                       /* RX Characteristic (Phone -> Device, Write) */
                       BT_GATT_CHARACTERISTIC(&shell_rx_char_uuid.uuid,
                                              BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                              BT_GATT_PERM_WRITE,
                                              NULL, shell_rx_write, NULL), );

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int bt_shell_init(void)
{
    if (shell_state.initialized)
    {
        return 0;
    }

    memset(&shell_state, 0, sizeof(shell_state));
    shell_state.initialized = true;

    LOG_INF("BT Shell service initialized");
    LOG_INF("  Service UUID: d5b1b7e2-7f5a-4eef-8fd0-1a2b3c4d5e71");
    LOG_INF("  TX Char UUID: d5b1b7e3-7f5a-4eef-8fd0-1a2b3c4d5e72 (Notify)");
    LOG_INF("  RX Char UUID: d5b1b7e4-7f5a-4eef-8fd0-1a2b3c4d5e73 (Write)");

    return 0;
}

int bt_shell_send_command(const char *cmd)
{
    if (!cmd)
    {
        return -EINVAL;
    }

    return bt_shell_send_data((const uint8_t *)cmd, strlen(cmd));
}

int bt_shell_send_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0)
    {
        return -EINVAL;
    }

    if (!bt_manager_is_connected())
    {
        LOG_WRN("Cannot send shell data: not connected");
        return -ENOTCONN;
    }

    if (!shell_state.notifications_enabled)
    {
        LOG_WRN("Cannot send shell data: notifications not enabled by peer");
        return -EACCES;
    }

    /* Send via notification on TX characteristic (attrs[2] is the value attribute) */
    int rc = bt_gatt_notify(NULL, &shell_svc.attrs[2], data, len);
    if (rc < 0)
    {
        LOG_ERR("Shell TX notify failed: %d", rc);
        return rc;
    }

    LOG_DBG("Shell TX sent %zu bytes", len);
    return 0;
}

bool bt_shell_notifications_enabled(void)
{
    return shell_state.notifications_enabled && bt_manager_is_connected();
}

void bt_shell_register_rx_callback(bt_shell_rx_callback_t callback)
{
    shell_state.rx_callback = callback;
}

#else /* !CONFIG_BT */

int bt_shell_init(void)
{
    LOG_WRN("BT Shell service not available (Bluetooth disabled)");
    return -ENOTSUP;
}

int bt_shell_send_command(const char *cmd)
{
    ARG_UNUSED(cmd);
    return -ENOTSUP;
}

int bt_shell_send_data(const uint8_t *data, size_t len)
{
    ARG_UNUSED(data);
    ARG_UNUSED(len);
    return -ENOTSUP;
}

bool bt_shell_notifications_enabled(void)
{
    return false;
}

void bt_shell_register_rx_callback(bt_shell_rx_callback_t callback)
{
    ARG_UNUSED(callback);
}

#endif /* CONFIG_BT */
