/**
 * @file bt_hid.c
 * @brief Bluetooth HID Service Implementation
 */

#include "bt_hid.h"
#include "bt_manager.h"
#include "../hid/hid_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#define BT_AVAILABLE 1
#else
#define BT_AVAILABLE 0
#endif

LOG_MODULE_REGISTER(bt_hid, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* HID Report Descriptors                                                    */
/*===========================================================================*/

/* Keyboard Report Descriptor */
static const uint8_t keyboard_report_desc[] = {
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
static const uint8_t gamepad_report_desc[] = {
    0x05, 0x01, /* Usage Page (Generic Desktop) */
    0x09, 0x05, /* Usage (Game Pad) */
    0xA1, 0x01, /* Collection (Application) */
    0x85, 0x02, /*   Report ID (2) */

    /* Axes */
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

    /* Buttons */
    0x05, 0x09, /*   Usage Page (Buttons) */
    0x19, 0x01, /*   Usage Min (1) */
    0x29, 0x10, /*   Usage Max (16) */
    0x15, 0x00, /*   Logical Min (0) */
    0x25, 0x01, /*   Logical Max (1) */
    0x75, 0x01, /*   Report Size (1) */
    0x95, 0x10, /*   Report Count (16) */
    0x81, 0x02, /*   Input (Data, Variable, Absolute) */

    /* Hat switch (D-pad) */
    0x05, 0x01,       /*   Usage Page (Generic Desktop) */
    0x09, 0x39,       /*   Usage (Hat Switch) */
    0x15, 0x00,       /*   Logical Min (0) */
    0x25, 0x07,       /*   Logical Max (7) */
    0x35, 0x00,       /*   Physical Min (0) */
    0x46, 0x3B, 0x01, /*   Physical Max (315) */
    0x65, 0x14,       /*   Unit (Degrees) */
    0x75, 0x04,       /*   Report Size (4) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x42,       /*   Input (Data, Variable, Null State) */
    0x75, 0x04,       /*   Report Size (4) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x01,       /*   Input (Constant) - padding */

    0xC0 /* End Collection */
};

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    bool enabled;
    hid_device_type_t device_types;

    hid_event_callback_t event_cb;
    void *event_cb_data;
    hid_output_callback_t output_cb;
    void *output_cb_data;
} bt_hid_state;

/*===========================================================================*/
/* BLE GATT Service (HIDS implementation)                                    */
/*===========================================================================*/

#if BT_AVAILABLE
/* Combined report map (keyboard + gamepad) */
static const uint8_t report_map[] = {
    /* Keyboard */
    /* (Report ID 1) */
    /* Included as-is from keyboard_report_desc */
};

/* We'll build a combined map at runtime by concatenating the two descriptors */
static const uint8_t *get_report_map(size_t *len)
{
    static uint8_t *map = NULL;
    static size_t map_len = 0;

    if (!map)
    {
        map_len = sizeof(keyboard_report_desc) + sizeof(gamepad_report_desc);
        map = k_malloc(map_len);
        if (!map)
        {
            map_len = 0;
            return NULL;
        }
        memcpy(map, keyboard_report_desc, sizeof(keyboard_report_desc));
        memcpy(map + sizeof(keyboard_report_desc), gamepad_report_desc, sizeof(gamepad_report_desc));
    }

    if (len)
        *len = map_len;
    return map;
}

/* HID Information: bcdHID = 0x0111, country = 0, flags = 0 */
static const uint8_t hid_info[] = {0x11, 0x01, 0x00, 0x00};

/* Current report state buffers */
static hid_keyboard_report_t current_keyboard = {0};
static hid_gamepad_report_t current_gamepad = {0};

/* Report reference descriptors */
static const uint8_t kb_report_ref[] = {0x01, 0x01}; /* ID=1, Type=input */
static const uint8_t gp_report_ref[] = {0x02, 0x01}; /* ID=2, Type=input */

/* Boot keyboard OUT to receive LED state from host */
static uint8_t boot_kb_out = 0;

/* CCC configs */
static struct bt_gatt_ccc_cfg kb_ccc_cfg[BT_GATT_CCC_MAX] = {};
static struct bt_gatt_ccc_cfg gp_ccc_cfg[BT_GATT_CCC_MAX] = {};

/* Simple read callbacks */
static ssize_t read_hid_info(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len,
                             uint16_t offset)
{
    const uint8_t *value = attr->user_data;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(hid_info));
}

static ssize_t read_report_map_cb(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr,
                                  void *buf, uint16_t len,
                                  uint16_t offset)
{
    size_t map_len;
    const uint8_t *map = get_report_map(&map_len);
    if (!map)
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, map, map_len);
}

static ssize_t read_kb_report(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_keyboard, sizeof(current_keyboard));
}

static ssize_t read_gp_report(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_gamepad, sizeof(current_gamepad));
}

static ssize_t read_report_ref(struct bt_conn *conn,
                               const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset)
{
    const uint8_t *ref = attr->user_data;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, ref, 2);
}

static ssize_t write_boot_kb_out(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len,
                                 uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (len < 1)
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);

    boot_kb_out = *((const uint8_t *)buf);

    LOG_INF("HID: Boot KB OUT (LEDs)=0x%02x", boot_kb_out);

    /* Notify higher layer (hid_manager) via output callback */
    if (bt_hid_state.output_cb)
    {
        bt_hid_state.output_cb(&boot_kb_out, 1, bt_hid_state.output_cb_data);
    }

    return len;
}

/* Service definition */
BT_GATT_SERVICE_DEFINE(hids_svc,
                       BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
                       BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ,
                                              BT_GATT_PERM_READ, read_hid_info, NULL, (void *)hid_info),
                       BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ,
                                              BT_GATT_PERM_READ, read_report_map_cb, NULL, NULL),

                       /* Keyboard report (ID = 1) */
                       BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ, read_kb_report, NULL, &current_keyboard),
                       BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ, read_report_ref, NULL, (void *)kb_report_ref),
                       BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

                       /* Gamepad report (ID = 2) */
                       BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ, read_gp_report, NULL, &current_gamepad),
                       BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ, read_report_ref, NULL, (void *)gp_report_ref),
                       BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

                       /* Boot Keyboard OUT (for LED feedback) */
                       BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_BOOT_KB_OUT_REPORT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                              BT_GATT_PERM_WRITE, NULL, write_boot_kb_out, NULL), );

/* Helper: find attribute by its user_data pointer */
static const struct bt_gatt_attr *find_attr_by_user_data(const void *user_data)
{
    const struct bt_gatt_attr *attrs = hids_svc.attrs;
    size_t count = hids_svc.attr_count;
    for (size_t i = 0; i < count; i++)
    {
        if (attrs[i].user_data == user_data)
            return &attrs[i];
    }
    return NULL;
}

#else  /* BT_AVAILABLE */
/* When BT not available, the service doesn't exist */
#endif /* BT_AVAILABLE */

/*===========================================================================*/
/* Transport Implementation                                                  */
/*===========================================================================*/

static int ble_hid_init(hid_device_type_t types)
{
    LOG_INF("BLE HID init (types=0x%02x)", types);

    bt_hid_state.device_types = types;
    bt_hid_state.initialized = true;

#if BT_AVAILABLE
    /* TODO: Register GATT services based on device types */
#endif

    return 0;
}

static int ble_hid_deinit(void)
{
    bt_hid_state.initialized = false;
    bt_hid_state.enabled = false;
    return 0;
}

static int ble_hid_enable(void)
{
    if (!bt_hid_state.initialized)
    {
        return -EINVAL;
    }

    bt_hid_state.enabled = true;

    /* Start advertising via BT manager */
    return bt_manager_start_advertising();
}

static int ble_hid_disable(void)
{
    bt_hid_state.enabled = false;
    return bt_manager_stop_advertising();
}

static int ble_hid_send_keyboard(const hid_keyboard_report_t *report)
{
    if (!bt_hid_state.enabled)
    {
        return -EINVAL;
    }

    if (!bt_manager_is_connected())
    {
        return -ENOTCONN;
    }

#if BT_AVAILABLE
    /* Copy into local buffer and notify connected peer(s) */
    memcpy(&current_keyboard, report, sizeof(current_keyboard));

    const struct bt_gatt_attr *attr = find_attr_by_user_data(&current_keyboard);
    if (!attr)
    {
        LOG_WRN("BLE HID: Keyboard attribute not found for notify");
        return -ENODEV;
    }

    int rc = bt_gatt_notify(NULL, attr, &current_keyboard, sizeof(current_keyboard));
    if (rc < 0)
    {
        LOG_ERR("BLE KB notify failed: %d", rc);
        return rc;
    }

    LOG_DBG("BLE KB notify sent: mod=%02x keys=[%02x %02x %02x %02x %02x %02x]",
            current_keyboard.modifiers,
            current_keyboard.keys[0], current_keyboard.keys[1], current_keyboard.keys[2],
            current_keyboard.keys[3], current_keyboard.keys[4], current_keyboard.keys[5]);
#endif

    return 0;
}

static int ble_hid_send_gamepad(const hid_gamepad_report_t *report)
{
    if (!bt_hid_state.enabled)
    {
        return -EINVAL;
    }

    if (!bt_manager_is_connected())
    {
        return -ENOTCONN;
    }

#if BT_AVAILABLE
    memcpy(&current_gamepad, report, sizeof(current_gamepad));

    const struct bt_gatt_attr *attr = find_attr_by_user_data(&current_gamepad);
    if (!attr)
    {
        LOG_WRN("BLE HID: Gamepad attribute not found for notify");
        return -ENODEV;
    }

    int rc = bt_gatt_notify(NULL, attr, &current_gamepad, sizeof(current_gamepad));
    if (rc < 0)
    {
        LOG_ERR("BLE GP notify failed: %d", rc);
        return rc;
    }

    LOG_DBG("BLE GP notify sent: btns=%04x hat=%d", current_gamepad.buttons, current_gamepad.hat);
#endif
    return 0;
}

static int ble_hid_register_event_cb(hid_event_callback_t cb, void *user_data)
{
    bt_hid_state.event_cb = cb;
    bt_hid_state.event_cb_data = user_data;
    return 0;
}

static int ble_hid_register_output_cb(hid_output_callback_t cb, void *user_data)
{
    bt_hid_state.output_cb = cb;
    bt_hid_state.output_cb_data = user_data;
    return 0;
}

static bool ble_hid_is_connected(void)
{
    return bt_manager_is_connected();
}

/*===========================================================================*/
/* Transport Operations Structure                                            */
/*===========================================================================*/

static const hid_transport_ops_t ble_hid_transport = {
    .name = "ble",
    .init = ble_hid_init,
    .deinit = ble_hid_deinit,
    .enable = ble_hid_enable,
    .disable = ble_hid_disable,
    .send_keyboard = ble_hid_send_keyboard,
    .send_gamepad = ble_hid_send_gamepad,
    .register_event_cb = ble_hid_register_event_cb,
    .register_output_cb = ble_hid_register_output_cb,
    .is_connected = ble_hid_is_connected};

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

static void bt_hid_bt_event_handler(bt_event_t event, void *data, void *user_data)
{
    ARG_UNUSED(data);
    ARG_UNUSED(user_data);

    if (!bt_hid_state.event_cb)
        return;

    switch (event)
    {
    case BT_EVENT_CONNECTED:
        bt_hid_state.event_cb(HID_EVENT_CONNECTED, NULL, bt_hid_state.event_cb_data);
        break;
    case BT_EVENT_DISCONNECTED:
        bt_hid_state.event_cb(HID_EVENT_DISCONNECTED, NULL, bt_hid_state.event_cb_data);
        break;
    default:
        break;
    }
}

int bt_hid_init(void)
{
    LOG_INF("Registering BLE HID transport");

    /* Register for BT manager events so we can forward connected/disconnected to HID manager */
    bt_manager_register_callback(bt_hid_bt_event_handler, NULL);

    return hid_manager_register_transport(&ble_hid_transport);
}

const hid_transport_ops_t *bt_hid_get_transport(void)
{
    return &ble_hid_transport;
}

int bt_hid_enable(void)
{
    return ble_hid_enable();
}

int bt_hid_disable(void)
{
    return ble_hid_disable();
}
