/**
 * @file bt_hid_service.c
 * @brief Bluetooth HID GATT Service Implementation
 */

#include "bt_hid_service.h"
#include "bt_manager.h"
#include "../hid/hid_manager.h"
#include "../hid/hid_keyboard.h"
#include "../hid/hid_gamepad.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/services/dis.h>
#define BT_AVAILABLE 1
#else
#define BT_AVAILABLE 0
#endif

LOG_MODULE_REGISTER(bt_hid_service, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* HID Report Descriptors                                                    */
/*===========================================================================*/

/* Keyboard Report Descriptor */
static const uint8_t keyboard_report_map[] = {
    0x05, 0x01, /* Usage Page (Generic Desktop) */
    0x09, 0x06, /* Usage (Keyboard) */
    0xA1, 0x01, /* Collection (Application) */
    0x85, 0x01, /*   Report ID (1) */

    /* Modifier keys */
    0x05, 0x07, /*   Usage Page (Key Codes) */
    0x19, 0xE0, /*   Usage Min (Left Control) */
    0x29, 0xE7, /*   Usage Max (Right GUI) */
    0x15, 0x00, /*   Logical Min (0) */
    0x25, 0x01, /*   Logical Max (1) */
    0x75, 0x01, /*   Report Size (1) */
    0x95, 0x08, /*   Report Count (8) */
    0x81, 0x02, /*   Input (Data, Variable, Absolute) */

    /* Reserved byte */
    0x75, 0x08, /*   Report Size (8) */
    0x95, 0x01, /*   Report Count (1) */
    0x81, 0x01, /*   Input (Constant) */

    /* LED output */
    0x05, 0x08, /*   Usage Page (LEDs) */
    0x19, 0x01, /*   Usage Min (Num Lock) */
    0x29, 0x05, /*   Usage Max (Kana) */
    0x75, 0x01, /*   Report Size (1) */
    0x95, 0x05, /*   Report Count (5) */
    0x91, 0x02, /*   Output (Data, Variable, Absolute) */
    0x75, 0x03, /*   Report Size (3) */
    0x95, 0x01, /*   Report Count (1) */
    0x91, 0x01, /*   Output (Constant) */

    /* Key array */
    0x05, 0x07,       /*   Usage Page (Key Codes) */
    0x19, 0x00,       /*   Usage Min (0) */
    0x29, 0xFF,       /*   Usage Max (255) */
    0x15, 0x00,       /*   Logical Min (0) */
    0x26, 0xFF, 0x00, /*   Logical Max (255) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x06,       /*   Report Count (6) */
    0x81, 0x00,       /*   Input (Data, Array) */

    0xC0 /* End Collection */
};

/* Gamepad Report Descriptor */
static const uint8_t gamepad_report_map[] = {
    0x05, 0x01, /* Usage Page (Generic Desktop) */
    0x09, 0x05, /* Usage (Game Pad) */
    0xA1, 0x01, /* Collection (Application) */
    0x85, 0x02, /*   Report ID (2) */

    /* Buttons */
    0x05, 0x09,       /*   Usage Page (Button) */
    0x19, 0x01,       /*   Usage Min (Button 1) */
    0x29, 0x10,       /*   Usage Max (Button 16) */
    0x15, 0x00,       /*   Logical Min (0) */
    0x25, 0x01,       /*   Logical Max (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x10,       /*   Report Count (16) */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute) */

    /* Axes */
    0x05, 0x01,       /*   Usage Page (Generic Desktop) */
    0x09, 0x30,       /*   Usage (X) */
    0x09, 0x31,       /*   Usage (Y) */
    0x09, 0x32,       /*   Usage (Z) - Right X */
    0x09, 0x35,       /*   Usage (Rz) - Right Y */
    0x16, 0x00, 0x80, /*   Logical Min (-32768) */
    0x26, 0xFF, 0x7F, /*   Logical Max (32767) */
    0x75, 0x10,       /*   Report Size (16) */
    0x95, 0x04,       /*   Report Count (4) */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute) */

    /* Triggers */
    0x09, 0x33,       /*   Usage (Rx) - Left Trigger */
    0x09, 0x34,       /*   Usage (Ry) - Right Trigger */
    0x16, 0x00, 0x80, /*   Logical Min (-32768) */
    0x26, 0xFF, 0x7F, /*   Logical Max (32767) */
    0x75, 0x10,       /*   Report Size (16) */
    0x95, 0x02,       /*   Report Count (2) */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute) */

    /* Hat/D-pad */
    0x09, 0x39,       /*   Usage (Hat Switch) */
    0x15, 0x00,       /*   Logical Min (0) */
    0x25, 0x07,       /*   Logical Max (7) */
    0x35, 0x00,       /*   Physical Min (0) */
    0x46, 0x3B, 0x01, /*   Physical Max (315) */
    0x65, 0x14,       /*   Unit (Degrees) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x42,       /*   Input (Data, Variable, Absolute, Null State) */

    0xC0 /* End Collection */
};

/*===========================================================================*/
/* HID Service State                                                         */
/*===========================================================================*/

#if BT_AVAILABLE

static struct {
    bool initialized;
    struct bt_conn *conn;
    bt_hid_protocol_mode_t protocol_mode;
    bool suspended;
    uint8_t battery_level;
    
    /* Notification enabled flags */
    bool keyboard_notify_enabled;
    bool gamepad_notify_enabled;
    
    struct k_mutex mutex;
} hid_service;

/*===========================================================================*/
/* HID Service UUIDs                                                         */
/*===========================================================================*/

/* HID Service (0x1812) */
#define BT_UUID_HIDS BT_UUID_DECLARE_16(0x1812)

/* HID Characteristics */
#define BT_UUID_HIDS_REPORT_MAP    BT_UUID_DECLARE_16(0x2A4B)
#define BT_UUID_HIDS_REPORT        BT_UUID_DECLARE_16(0x2A4D)
#define BT_UUID_HIDS_PROTOCOL_MODE BT_UUID_DECLARE_16(0x2A4E)
#define BT_UUID_HIDS_CONTROL_POINT BT_UUID_DECLARE_16(0x2A4C)
#define BT_UUID_HIDS_INFO          BT_UUID_DECLARE_16(0x2A4A)

/* Report Reference Descriptor */
#define BT_UUID_HIDS_REPORT_REF    BT_UUID_DECLARE_16(0x2908)

/*===========================================================================*/
/* GATT Callbacks                                                            */
/*===========================================================================*/

static ssize_t read_report_map(struct bt_conn *conn,
                               const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset)
{
    /* Return combined report map (keyboard + gamepad) */
    const uint8_t *map = keyboard_report_map;
    size_t map_len = sizeof(keyboard_report_map);
    
    /* TODO: Combine both maps if both profiles enabled */
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, map, map_len);
}

static ssize_t read_hid_info(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    /* HID Information: bcdHID=1.11, bCountryCode=0, Flags=RemoteWake|NormallyConnectable */
    static const uint8_t hid_info[] = {0x11, 0x01, 0x00, 0x03};
    return bt_gatt_attr_read(conn, attr, buf, len, offset, hid_info, sizeof(hid_info));
}

static ssize_t read_protocol_mode(struct bt_conn *conn,
                                   const struct bt_gatt_attr *attr,
                                   void *buf, uint16_t len, uint16_t offset)
{
    uint8_t mode = hid_service.protocol_mode;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &mode, sizeof(mode));
}

static ssize_t write_protocol_mode(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    const void *buf, uint16_t len,
                                    uint16_t offset, uint8_t flags)
{
    if (offset + len > sizeof(uint8_t)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    uint8_t mode = *((uint8_t *)buf);
    if (mode > BT_HID_PROTOCOL_REPORT) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    hid_service.protocol_mode = mode;
    LOG_INF("Protocol mode changed to: %s", mode ? "Report" : "Boot");
    
    return len;
}

static ssize_t write_control_point(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    const void *buf, uint16_t len,
                                    uint16_t offset, uint8_t flags)
{
    if (offset + len > sizeof(uint8_t)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    uint8_t cmd = *((uint8_t *)buf);
    
    switch (cmd) {
    case BT_HID_CP_SUSPEND:
        hid_service.suspended = true;
        LOG_INF("HID suspended");
        break;
    case BT_HID_CP_EXIT_SUSPEND:
        hid_service.suspended = false;
        LOG_INF("HID resumed");
        break;
    default:
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    return len;
}

static void keyboard_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    hid_service.keyboard_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Keyboard notifications %s", hid_service.keyboard_notify_enabled ? "enabled" : "disabled");
}

static void gamepad_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    hid_service.gamepad_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Gamepad notifications %s", hid_service.gamepad_notify_enabled ? "enabled" : "disabled");
}

/*===========================================================================*/
/* Report Reference Descriptors                                              */
/*===========================================================================*/

/* Keyboard Input Report Reference */
static const uint8_t keyboard_input_report_ref[] = {0x01, 0x01}; /* Report ID 1, Input */

/* Gamepad Input Report Reference */
static const uint8_t gamepad_input_report_ref[] = {0x02, 0x01};  /* Report ID 2, Input */

/*===========================================================================*/
/* HID Service Definition                                                    */
/*===========================================================================*/

BT_GATT_SERVICE_DEFINE(hids,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
    
    /* HID Information */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO,
                          BT_GATT_CHRC_READ,
                          BT_GATT_PERM_READ,
                          read_hid_info, NULL, NULL),
    
    /* Report Map */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP,
                          BT_GATT_CHRC_READ,
                          BT_GATT_PERM_READ,
                          read_report_map, NULL, NULL),
    
    /* Protocol Mode */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_protocol_mode, write_protocol_mode, NULL),
    
    /* Control Point */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CONTROL_POINT,
                          BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                          BT_GATT_PERM_WRITE,
                          NULL, write_control_point, NULL),
    
    /* Keyboard Input Report */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ,
                          NULL, NULL, NULL),
    BT_GATT_CCC(keyboard_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
                      BT_GATT_PERM_READ,
                      bt_gatt_attr_read, NULL, (void *)keyboard_input_report_ref),
    
    /* Gamepad Input Report */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ,
                          NULL, NULL, NULL),
    BT_GATT_CCC(gamepad_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
                      BT_GATT_PERM_READ,
                      bt_gatt_attr_read, NULL, (void *)gamepad_input_report_ref),
);

/*===========================================================================*/
/* Connection Callbacks                                                      */
/*===========================================================================*/

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed (err %u)", err);
        return;
    }

    k_mutex_lock(&hid_service.mutex, K_FOREVER);
    hid_service.conn = bt_conn_ref(conn);
    k_mutex_unlock(&hid_service.mutex);

    LOG_INF("HID connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    k_mutex_lock(&hid_service.mutex, K_FOREVER);
    
    if (hid_service.conn == conn) {
        bt_conn_unref(hid_service.conn);
        hid_service.conn = NULL;
        hid_service.keyboard_notify_enabled = false;
        hid_service.gamepad_notify_enabled = false;
    }
    
    k_mutex_unlock(&hid_service.mutex);

    LOG_INF("HID disconnected (reason %u)", reason);
}

BT_CONN_CB_DEFINE(hid_conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/*===========================================================================*/
/* Public API Implementation                                                 */
/*===========================================================================*/

int bt_hid_service_init(void)
{
    if (hid_service.initialized) {
        return 0;
    }

    LOG_INF("Initializing BT HID service");

    k_mutex_init(&hid_service.mutex);
    
    hid_service.conn = NULL;
    hid_service.protocol_mode = BT_HID_PROTOCOL_REPORT;
    hid_service.suspended = false;
    hid_service.battery_level = 100;
    hid_service.keyboard_notify_enabled = false;
    hid_service.gamepad_notify_enabled = false;

    hid_service.initialized = true;
    return 0;
}

int bt_hid_service_register(void)
{
    if (!hid_service.initialized) {
        return -ENODEV;
    }

    /* Service is auto-registered via BT_GATT_SERVICE_DEFINE */
    LOG_INF("BT HID service registered");
    return 0;
}

int bt_hid_service_send_keyboard_report(const hid_keyboard_report_t *report)
{
    if (!hid_service.initialized || !report) {
        return -EINVAL;
    }

    if (!hid_service.conn || !hid_service.keyboard_notify_enabled) {
        return -ENOTCONN;
    }

    if (hid_service.suspended) {
        return -EAGAIN;
    }

    /* Find keyboard report attribute (index 10 in service) */
    const struct bt_gatt_attr *attr = &hids.attrs[10];
    
    int err = bt_gatt_notify(hid_service.conn, attr, report, sizeof(hid_keyboard_report_t));
    if (err) {
        LOG_ERR("Failed to send keyboard report (err %d)", err);
        return err;
    }

    return 0;
}

int bt_hid_service_send_gamepad_report(const hid_gamepad_report_t *report)
{
    if (!hid_service.initialized || !report) {
        return -EINVAL;
    }

    if (!hid_service.conn || !hid_service.gamepad_notify_enabled) {
        return -ENOTCONN;
    }

    if (hid_service.suspended) {
        return -EAGAIN;
    }

    /* Find gamepad report attribute (index 13 in service) */
    const struct bt_gatt_attr *attr = &hids.attrs[13];
    
    int err = bt_gatt_notify(hid_service.conn, attr, report, sizeof(hid_gamepad_report_t));
    if (err) {
        LOG_ERR("Failed to send gamepad report (err %d)", err);
        return err;
    }

    return 0;
}

bool bt_hid_service_is_connected(void)
{
    k_mutex_lock(&hid_service.mutex, K_FOREVER);
    bool connected = (hid_service.conn != NULL);
    k_mutex_unlock(&hid_service.mutex);
    return connected;
}

bt_hid_protocol_mode_t bt_hid_service_get_protocol_mode(void)
{
    return hid_service.protocol_mode;
}

int bt_hid_service_set_battery_level(uint8_t level)
{
    if (level > 100) {
        return -EINVAL;
    }

    hid_service.battery_level = level;
    
    /* Update BAS if available */
    #ifdef CONFIG_BT_BAS
    bt_bas_set_battery_level(level);
    #endif
    
    return 0;
}

void *bt_hid_service_get_conn(void)
{
    return hid_service.conn;
}

#else /* !BT_AVAILABLE */

/* Stub implementations when Bluetooth is disabled */
int bt_hid_service_init(void) { return -ENOTSUP; }
int bt_hid_service_register(void) { return -ENOTSUP; }
int bt_hid_service_send_keyboard_report(const hid_keyboard_report_t *r) { return -ENOTSUP; }
int bt_hid_service_send_gamepad_report(const hid_gamepad_report_t *r) { return -ENOTSUP; }
bool bt_hid_service_is_connected(void) { return false; }
bt_hid_protocol_mode_t bt_hid_service_get_protocol_mode(void) { return BT_HID_PROTOCOL_REPORT; }
int bt_hid_service_set_battery_level(uint8_t level) { return -ENOTSUP; }
void *bt_hid_service_get_conn(void) { return NULL; }

#endif /* BT_AVAILABLE */
