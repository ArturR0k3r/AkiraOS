/**
 * @file bt_echo.c
 * @brief Simple Bluetooth Echo GATT service
 *
 * When enabled, accepts writes to the Echo characteristic and replies via
 * notification with the same payload. Useful for verifying connection and
 * data flow without touching OTA/HID features.
 */

#include "bt_echo.h"
#include <zephyr/logging/log.h>
#if defined(CONFIG_BT)
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/bluetooth.h>
#endif

LOG_MODULE_REGISTER(bt_echo, CONFIG_AKIRA_LOG_LEVEL);

#if defined(CONFIG_BT)

/* 128-bit UUID: d5b1b7e0-7f5a-4eef-8fd0-1a2b3c4d5e6f */
static struct bt_uuid_128 echo_service_uuid = BT_UUID_INIT_128(
    0x6f,0x5e,0x4d,0x3c,0x2b,0x1a,0xd0,0x8f,0xef,0x4e,0x5a,0x7f,0xe0,0xb7,0xb1,0xd5);
static struct bt_uuid_128 echo_char_uuid = BT_UUID_INIT_128(
    0x70,0x5e,0x4d,0x3c,0x2b,0x1a,0xd0,0x8f,0xef,0x4e,0x5a,0x7f,0xe1,0xb7,0xb1,0xd5);

static bool g_enabled = true;

static ssize_t echo_write(struct bt_conn *conn,
                          const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len,
                          uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (!g_enabled)
    {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }

    if (len == 0)
        return 0;

    const uint8_t *data = buf;

    /* Notify same payload back to connected peer(s) */
    int rc = bt_gatt_notify(NULL, attr, data, len);
    if (rc < 0)
    {
        LOG_ERR("Echo notify failed: %d", rc);
    }

    /* Show received payload at INFO level for easier testing */
    char hex[129] = {0};
    int hpos = 0;
    for (int i = 0; i < len && i < 64; i++) {
        hpos += snprintf(hex + hpos, sizeof(hex) - hpos, "%02x ", data[i]);
    }
    if (len > 64) {
        snprintf(hex + hpos, sizeof(hex) - hpos, "... (%d bytes total)", len);
    }
    LOG_INF("Echo write received (%d bytes): %s", len, hex);
    return len;
}

/* Characteristic + CCC for notify */
BT_GATT_SERVICE_DEFINE(echo_svc,
    BT_GATT_PRIMARY_SERVICE(&echo_service_uuid.uuid),
    BT_GATT_CHARACTERISTIC(&echo_char_uuid.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE,
                           NULL, echo_write, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

int bt_echo_init(void)
{
    LOG_INF("BT Echo service %s", g_enabled ? "enabled" : "disabled");
    return 0;
}

void bt_echo_enable(bool enable)
{
    g_enabled = enable;
    LOG_INF("BT Echo %s", enable ? "enabled" : "disabled");
}

bool bt_echo_is_enabled(void)
{
    return g_enabled;
}

#else

int bt_echo_init(void)
{
    return -ENOTSUP;
}

void bt_echo_enable(bool enable)
{
    ARG_UNUSED(enable);
}

bool bt_echo_is_enabled(void)
{
    return false;
}

#endif /* CONFIG_BT */
