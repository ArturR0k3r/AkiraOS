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

#define DIS_SVC_UUID    BT_UUID_DECLARE_16(0x180A)
#define PNP_ID_UUID     BT_UUID_DECLARE_16(0x2A50)

#if CONFIG_AKIRA_HID_GAMEPAD

/* Reports as a real Xbox Wireless Controller (model 1708): host frameworks
 * that pattern-match known controller identities (e.g. GCExtendedGamepad on
 * iOS/macOS/tvOS) require this exact VID/PID to grant full-featured gamepad
 * support rather than falling back to generic/partial HID mapping. */
static const uint8_t gamepad_pnp_id[] = {
    0x02,             /* VID Source: USB */
    0x5E, 0x04,       /* VID: 0x045E (Microsoft) LE */
    0xE0, 0x02,       /* PID: 0x02E0 (model 1708 BLE) LE */
    0x11, 0x01        /* Version: 0x0111 LE */
};

static ssize_t read_pnp_id(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             gamepad_pnp_id, sizeof(gamepad_pnp_id));
}

/* Device Information Service */
BT_GATT_SERVICE_DEFINE(dis_svc,
    BT_GATT_PRIMARY_SERVICE(DIS_SVC_UUID),
    BT_GATT_CHARACTERISTIC(PNP_ID_UUID,
        BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ,
        read_pnp_id, NULL, NULL),
);

#endif /* CONFIG_AKIRA_HID_GAMEPAD */

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

/* Gamepad Report Descriptor — Xbox Wireless Controller (model 1708) layout.
 * Report ID reused as 1 (safe: AKIRA_HID_MODE_GAMEPAD is exclusive with
 * keyboard/mouse/consumer via the AKIRA_HID_MODE choice, so no ID 1 clash). */
static const uint8_t gamepad_report_desc[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x05,       /* Usage (Game Pad) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x01,       /*   Report ID (1) */

    /* ── Pointer physical collection (axes + hat + buttons) ── */
    0x09, 0x01,       /*   Usage (Pointer) */
    0xA1, 0x00,       /*   Collection (Physical) */

    /* 10 buttons */
    0x05, 0x09,       /*     Usage Page (Buttons) */
    0x19, 0x01,       /*     Usage Min (1) */
    0x29, 0x0A,       /*     Usage Max (10) */
    0x15, 0x00,       /*     Logical Min (0) */
    0x25, 0x01,       /*     Logical Max (1) */
    0x75, 0x01,       /*     Report Size (1) */
    0x95, 0x0A,       /*     Report Count (10) */
    0x81, 0x02,       /*     Input (Data,Var,Abs) */

    /* 4 axes — unsigned 16-bit, centre=32768 */
    0x05, 0x01,       /*     Usage Page (Generic Desktop) */
    0x09, 0x30,       /*     Usage (X) */
    0x09, 0x31,       /*     Usage (Y) */
    0x09, 0x32,       /*     Usage (Z)  — right X */
    0x09, 0x35,       /*     Usage (Rz) — right Y */
    0x15, 0x00,       /*     Logical Min (0) */
    0x27, 0xFF, 0xFF, 0x00, 0x00, /* Logical Max (65535) */
    0x75, 0x10,       /*     Report Size (16) */
    0x95, 0x04,       /*     Report Count (4) */
    0x81, 0x02,       /*     Input (Data,Var,Abs) */

    /* Hat switch */
    0x09, 0x39,       /*     Usage (Hat switch) */
    0x15, 0x00,       /*     Logical Min (0) */
    0x25, 0x07,       /*     Logical Max (7) */
    0x35, 0x00,       /*     Physical Min (0) */
    0x46, 0x3B, 0x01, /*     Physical Max (315) */
    0x65, 0x14,       /*     Unit (deg) */
    0x75, 0x04,       /*     Report Size (4) */
    0x95, 0x01,       /*     Report Count (1) */
    0x81, 0x42,       /*     Input (Data,Var,Abs,Null) */
    0x75, 0x04,       /*     Report Size (4) */
    0x95, 0x01,       /*     Report Count (1) */
    0x81, 0x01,       /*     Input (Const) — 4-bit pad */

    0xC0,             /*   End Collection (Physical) */

    /* ── Triggers (Simulation Controls page, 10-bit) ── */
    0x05, 0x02,       /*   Usage Page (Simulation Controls) */
    0x09, 0xC5,       /*   Usage (Brake) — Left Trigger */
    0x15, 0x00,       /*   Logical Min (0) */
    0x26, 0xFF, 0x03, /*   Logical Max (1023) */
    0x75, 0x0A,       /*   Report Size (10) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x09, 0xC4,       /*   Usage (Accelerator) — Right Trigger */
    0x15, 0x00,       /*   Logical Min (0) */
    0x26, 0xFF, 0x03, /*   Logical Max (1023) */
    0x75, 0x0A,       /*   Report Size (10) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x75, 0x06,       /*   Report Size (6) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x01,       /*   Input (Const) — 6-bit pad */

    0xC0              /* End Collection (Application) */
};

/* Mouse Report Descriptor (Report ID 3)
 * Layout: buttons[u8], dx[s8], dy[s8], wheel[s8] — matches hid_mouse_report_t */
static const uint8_t mouse_report_desc[] = {
    0x05, 0x01, /* Usage Page (Generic Desktop) */
    0x09, 0x02, /* Usage (Mouse) */
    0xA1, 0x01, /* Collection (Application) */
    0x85, 0x03, /*   Report ID (3) */
    0x09, 0x01, /*   Usage (Pointer) */
    0xA1, 0x00, /*   Collection (Physical) */

    /* Buttons: 5 usable + 3 padding bits = 1 byte */
    0x05, 0x09, /*     Usage Page (Buttons) */
    0x19, 0x01, /*     Usage Min (1) */
    0x29, 0x05, /*     Usage Max (5) */
    0x15, 0x00, /*     Logical Min (0) */
    0x25, 0x01, /*     Logical Max (1) */
    0x75, 0x01, /*     Report Size (1) */
    0x95, 0x05, /*     Report Count (5) */
    0x81, 0x02, /*     Input (Data, Variable, Absolute) */
    0x75, 0x03, /*     Report Size (3) - padding */
    0x95, 0x01, /*     Report Count (1) */
    0x81, 0x01, /*     Input (Constant) */

    /* X, Y axes: signed 8-bit */
    0x05, 0x01, /*     Usage Page (Generic Desktop) */
    0x09, 0x30, /*     Usage (X) */
    0x09, 0x31, /*     Usage (Y) */
    0x15, 0x81, /*     Logical Min (-127) */
    0x25, 0x7F, /*     Logical Max (127) */
    0x75, 0x08, /*     Report Size (8) */
    0x95, 0x02, /*     Report Count (2) */
    0x81, 0x06, /*     Input (Data, Variable, Relative) */

    /* Wheel: signed 8-bit */
    0x09, 0x38, /*     Usage (Wheel) */
    0x15, 0x81, /*     Logical Min (-127) */
    0x25, 0x7F, /*     Logical Max (127) */
    0x75, 0x08, /*     Report Size (8) */
    0x95, 0x01, /*     Report Count (1) */
    0x81, 0x06, /*     Input (Data, Variable, Relative) */

    0xC0,       /*   End Collection (Physical) */
    0xC0        /* End Collection (Application) */
};

/* Consumer Report Descriptor (Report ID 4)
 * Layout: usage[u16 LE] — matches hid_consumer_report_t */
static const uint8_t consumer_report_desc[] = {
    0x05, 0x0C,       /* Usage Page (Consumer Devices) */
    0x09, 0x01,       /* Usage (Consumer Control) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x04,       /*   Report ID (4) */
    0x19, 0x00,       /*   Usage Min (0) */
    0x2A, 0xFF, 0x02, /*   Usage Max (0x02FF) */
    0x15, 0x00,       /*   Logical Min (0) */
    0x26, 0xFF, 0x02, /*   Logical Max (0x02FF) */
    0x75, 0x10,       /*   Report Size (16) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x00,       /*   Input (Data, Array, Absolute) */
    0xC0              /* End Collection */
};

/* Combined static report map (all 4 descriptors back-to-back) */
static const uint8_t combined_report_map[] = {
#if CONFIG_AKIRA_HID_KEYBOARD
    /* Keyboard (ID 1) */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x75, 0x01,
    0x95, 0x05, 0x91, 0x02, 0x75, 0x03, 0x95, 0x01,
    0x91, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x29, 0xFF, 0x15, 0x00,
    0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00,
    0xC0,
#endif /* CONFIG_AKIRA_HID_KEYBOARD */

#if CONFIG_AKIRA_HID_GAMEPAD
    /* Gamepad (ID 1) — byte-exact Xbox Wireless Controller (model 1708)
     * report map, required for host frameworks (e.g. GCExtendedGamepad)
     * that pattern-match known controller descriptors rather than parsing
     * generically. See gamepad_to_wire() for the matching wire layout. */
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x01, 0xA1, 0x00, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x95,
    0x02, 0x75, 0x10, 0x81, 0x02, 0xC0,
    0x09, 0x01, 0xA1, 0x00, 0x09, 0x32, 0x09, 0x35,
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x95,
    0x02, 0x75, 0x10, 0x81, 0x02, 0xC0,
    0x05, 0x02, 0x09, 0xC5, 0x15, 0x00, 0x26, 0xFF,
    0x03, 0x95, 0x01, 0x75, 0x0A, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x02, 0x09, 0xC4, 0x15, 0x00, 0x26, 0xFF,
    0x03, 0x95, 0x01, 0x75, 0x0A, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08,
    0x35, 0x00, 0x46, 0x3B, 0x01, 0x66, 0x14, 0x00,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x75, 0x04, 0x95, 0x01, 0x15, 0x00, 0x25, 0x00,
    0x35, 0x00, 0x45, 0x00, 0x65, 0x00, 0x81, 0x03,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0F,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x01, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x0C, 0x0A, 0xB2, 0x00,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x01, 0x75, 0x01, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03,
    0xC0,
#endif /* CONFIG_AKIRA_HID_GAMEPAD */

#if CONFIG_AKIRA_HID_MOUSE
    /* Mouse (ID 3) */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x03,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x05, 0x81, 0x02,
    0x75, 0x03, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
#endif /* CONFIG_AKIRA_HID_MOUSE */

#if CONFIG_AKIRA_HID_CONSUMER
    /* Consumer (ID 4) */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x04,
    0x19, 0x00, 0x2A, 0xFF, 0x02,
    0x15, 0x00, 0x26, 0xFF, 0x02,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0
#endif /* CONFIG_AKIRA_HID_CONSUMER */
};

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct {
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
    if (len)
        *len = sizeof(combined_report_map);
    return combined_report_map;
}

/* HID Information: bcdHID=0x0111, country=0,
 * flags=0x02 (NormallyConnectable) — iOS checks bit1 during HOGP discovery */
static const uint8_t hid_info[] = {0x11, 0x01, 0x00, 0x02};

/* Protocol Mode: 0x01 = Report Protocol (default), 0x00 = Boot Protocol.
 * Required as first HIDS characteristic by iOS HOGP spec (HOGS 1.1 §3.2). */
static uint8_t protocol_mode = 0x01;

/* Current report state buffers */
static hid_keyboard_report_t current_keyboard = {0};
static uint8_t ble_gamepad_report[16] = {0}; /* no Report ID — conveyed via Report Reference descriptor */
static hid_mouse_report_t current_mouse = {0};
static hid_consumer_report_t current_consumer = {0};

#define BLE_HID_GAMEPAD_REPORT_SIZE 16

/* Report reference descriptors — {report_id, report_type(1=input)} */
static const uint8_t kb_report_ref[] = {0x01, 0x01}; /* ID=1, Type=input */
static const uint8_t kb_out_report_ref[] = {0x01, 0x02}; /* ID=1, Type=output */
static const uint8_t gp_report_ref[] = {0x01, 0x01}; /* ID=1, Type=input */
static const uint8_t ms_report_ref[] = {0x03, 0x01}; /* ID=3, Type=input */
static const uint8_t cs_report_ref[] = {0x04, 0x01}; /* ID=4, Type=input */

/* Boot keyboard OUT to receive LED state from host */
static uint8_t boot_kb_out = 0;

/* Boot keyboard IN to send key state to host */
static uint8_t boot_kb_in[8] = {0};

/* Current keyboard LED state */
static uint8_t current_kb_leds = 0;

/* CCC configs (one per notifying characteristic) */
static struct bt_gatt_ccc_cfg kb_ccc_cfg[BT_GATT_CCC_MAX] = {};
static struct bt_gatt_ccc_cfg gp_ccc_cfg[BT_GATT_CCC_MAX] = {};
static struct bt_gatt_ccc_cfg ms_ccc_cfg[BT_GATT_CCC_MAX] = {};
static struct bt_gatt_ccc_cfg cs_ccc_cfg[BT_GATT_CCC_MAX] = {};

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
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_keyboard,
                             sizeof(current_keyboard));
}

static ssize_t read_gp_report(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, ble_gamepad_report,
                             sizeof(ble_gamepad_report));
}

static ssize_t read_ms_report(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_mouse,
                             sizeof(current_mouse));
}

static ssize_t read_cs_report(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_consumer,
                             sizeof(current_consumer));
}

static ssize_t read_report_ref(struct bt_conn *conn,
                               const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset)
{
    const uint8_t *ref = attr->user_data;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, ref, 2);
}

static ssize_t read_protocol_mode(struct bt_conn *conn,
                                   const struct bt_gatt_attr *attr,
                                   void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &protocol_mode, sizeof(protocol_mode));
}

static ssize_t write_protocol_mode(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    const void *buf, uint16_t len,
                                    uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (len != 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    /* We always stay in Report Protocol; accept but ignore Boot Protocol switch */
    uint8_t requested = *((const uint8_t *)buf);
    LOG_DBG("HID: Protocol Mode write 0x%02x (ignored — staying in Report Protocol)",
            requested);
    return len;
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
    if (bt_hid_state.output_cb) {
        bt_hid_state.output_cb(&boot_kb_out, 1, bt_hid_state.output_cb_data);
    }

    return len;
}

static ssize_t write_control_point(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    const void *buf, uint16_t len,
                                    uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (len != 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    uint8_t cmd = *((const uint8_t *)buf);
    /* 0x00 = Suspend, 0x01 = Exit Suspend */
    LOG_DBG("HID: Control Point cmd=0x%02x (%s)", cmd,
            cmd == 0x00 ? "Suspend" : (cmd == 0x01 ? "ExitSuspend" : "?"));
    return len;
}

static ssize_t read_boot_kb_in(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, boot_kb_in, sizeof(boot_kb_in));
}

static ssize_t read_kb_out_report(struct bt_conn *conn,
                                   const struct bt_gatt_attr *attr,
                                   void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &current_kb_leds, sizeof(current_kb_leds));
}

static ssize_t write_kb_out_report(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    const void *buf, uint16_t len,
                                    uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (len < 1){
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    current_kb_leds = *((const uint8_t *)buf);
    LOG_INF("HID: KB LED state=0x%02x", current_kb_leds);

    if (bt_hid_state.output_cb)
    {
        bt_hid_state.output_cb(&current_kb_leds, 1, bt_hid_state.output_cb_data);
    }

    return len;
}

/* CCC (Client Characteristic Configuration) change callbacks */
static void kb_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    LOG_DBG("KB CCC: %s", value == BT_GATT_CCC_NOTIFY ? "subscribed" : "unsubscribed");
}

static void gp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    LOG_DBG("GP CCC: %s", value == BT_GATT_CCC_NOTIFY ? "subscribed" : "unsubscribed");
}

static void ms_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    LOG_DBG("MS CCC: %s", value == BT_GATT_CCC_NOTIFY ? "subscribed" : "unsubscribed");
}

static void cs_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    LOG_DBG("CS CCC: %s", value == BT_GATT_CCC_NOTIFY ? "subscribed" : "unsubscribed");
}

static void boot_kb_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    LOG_DBG("Boot KB CCC: %s", value == BT_GATT_CCC_NOTIFY ? "subscribed" : "unsubscribed");
}

/* Service definition
 * Attribute order follows HOGS 1.1 §3.1 (iOS enforces this):
 *   1. Protocol Mode     — plain perm (spec §3.2)
 *   2. HID Information   — encrypted read
 *   3. Report Map        — encrypted read
 *   4–N. Report chars    — encrypted read/notify + plain CCC
 *   N+1. Boot KB OUT     — encrypted write
 *   Last. Control Point  — plain write (mandatory, HOGS §3.4.3; iOS skips HOGP without it)
 */
BT_GATT_SERVICE_DEFINE(hids_svc,
                        BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),

                       /* Protocol Mode — MUST be first char; plain perm per HOGP spec */
                        BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE,
                                            BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                            BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                                            read_protocol_mode, write_protocol_mode,
                                            &protocol_mode),

                        BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ,
                                            BT_GATT_PERM_READ_ENCRYPT, read_hid_info, NULL,
                                              (void *)hid_info),

                        BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ,
                                            BT_GATT_PERM_READ_ENCRYPT, read_report_map_cb,
                                            NULL, NULL),

#if CONFIG_AKIRA_HID_KEYBOARD
                       /* Keyboard INPUT report (ID=1) */
                        BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                                                BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                                BT_GATT_PERM_READ_ENCRYPT,
                                                read_kb_report, NULL, &current_keyboard),
                        BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
                                            BT_GATT_PERM_READ_ENCRYPT,
                                          read_report_ref, NULL, (void *)kb_report_ref),
                        BT_GATT_CCC(kb_ccc_changed, BT_GATT_PERM_READ_ENCRYPT |
                                                    BT_GATT_PERM_WRITE_ENCRYPT),

                       /* Keyboard OUTPUT report — LED state from host (ID=1) */
                        BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                                        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
                                        BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                        BT_GATT_PERM_READ_ENCRYPT |
                                        BT_GATT_PERM_WRITE_ENCRYPT,
                                        read_kb_out_report, write_kb_out_report,
                                        &current_kb_leds),
                        BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
                                        BT_GATT_PERM_READ_ENCRYPT,
                                          read_report_ref, NULL, (void *)kb_out_report_ref),
#endif /* CONFIG_AKIRA_HID_KEYBOARD */

#if CONFIG_AKIRA_HID_GAMEPAD
                       /* Gamepad report (ID = 1) */
                        BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                                            BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                            BT_GATT_PERM_READ_ENCRYPT, read_gp_report, NULL,
                                            ble_gamepad_report),
                        BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
                                        BT_GATT_PERM_READ_ENCRYPT, read_report_ref, NULL,
                                          (void *)gp_report_ref),
                        BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
#endif /* CONFIG_AKIRA_HID_GAMEPAD */

#if CONFIG_AKIRA_HID_MOUSE
                       /* Mouse report (ID = 3) */
                        BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                                            BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                            BT_GATT_PERM_READ_ENCRYPT, read_ms_report, NULL,
                                            &current_mouse),
                        BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
                                        BT_GATT_PERM_READ_ENCRYPT, read_report_ref, NULL,
                                          (void *)ms_report_ref),
                        BT_GATT_CCC(ms_ccc_changed, BT_GATT_PERM_READ_ENCRYPT |
                                                BT_GATT_PERM_WRITE_ENCRYPT),
#endif /* CONFIG_AKIRA_HID_MOUSE */

#if CONFIG_AKIRA_HID_CONSUMER
                       /* Consumer / Media keys report (ID = 4) */
                        BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                                            BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                            BT_GATT_PERM_READ_ENCRYPT, read_cs_report, NULL,
                                            &current_consumer),
                        BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
                                        BT_GATT_PERM_READ_ENCRYPT, read_report_ref, NULL,
                                          (void *)cs_report_ref),
                        BT_GATT_CCC(cs_ccc_changed, BT_GATT_PERM_READ_ENCRYPT |
                                                BT_GATT_PERM_WRITE_ENCRYPT),
#endif /* CONFIG_AKIRA_HID_CONSUMER */

#if CONFIG_AKIRA_HID_KEYBOARD
                        BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_BOOT_KB_IN_REPORT,
                                            BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                            BT_GATT_PERM_READ_ENCRYPT,
                                            read_boot_kb_in, NULL, boot_kb_in),
                        BT_GATT_CCC(boot_kb_ccc_changed, BT_GATT_PERM_READ_ENCRYPT |
                                                    BT_GATT_PERM_WRITE_ENCRYPT),
                        /* Boot Keyboard OUT (for LED feedback) */
                        BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_BOOT_KB_OUT_REPORT,
                                            BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                            BT_GATT_PERM_WRITE_ENCRYPT, NULL,
                                            write_boot_kb_out, NULL),
#endif /* CONFIG_AKIRA_HID_KEYBOARD */
                        /* Control Point — mandatory per HOGS 1.1 §3.4.3; iOS won't finish HOGP
                        * enumeration without it. Plain write perm (spec allows no security). */
                        BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT,
                                            BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                            BT_GATT_PERM_WRITE, NULL, write_control_point, NULL));

/* Helper: find attribute by its user_data pointer */
static const struct bt_gatt_attr *find_attr_by_user_data(const void *user_data)
{
    const struct bt_gatt_attr *attrs = hids_svc.attrs;
    size_t count = hids_svc.attr_count;
    for (size_t i = 0; i < count; i++) {
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
    if (!bt_hid_state.initialized) {
        return -EINVAL;
    }

    bt_hid_state.enabled = true;

    /* Start advertising via BT manager */
    int ret = bt_manager_start_advertising();
    if (ret < 0 && ret != -EBUSY && ret != -EALREADY) {
        LOG_WRN("Failed to start advertising via BT Manager (%d)", ret);
        return ret;
    }
    return 0;
}

static int ble_hid_disable(void)
{
    bt_hid_state.enabled = false;
    return bt_manager_stop_advertising();
}

static int ble_hid_send_keyboard(const hid_keyboard_report_t *report)
{
    if (!bt_hid_state.enabled) {
        return -EINVAL;
    }

    if (!bt_manager_is_connected()) {
        return -ENOTCONN;
    }

#if BT_AVAILABLE
    /* Copy into local buffer and notify connected peer(s) */
    memcpy(&current_keyboard, report, sizeof(current_keyboard));

    const struct bt_gatt_attr *attr = find_attr_by_user_data(&current_keyboard);
    if (!attr) {
        LOG_WRN("BLE HID: Keyboard attribute not found for notify");
        return -ENODEV;
    }

    int rc = bt_gatt_notify(NULL, attr, &current_keyboard, sizeof(current_keyboard));
    if (rc < 0) {
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

/* Converts the internal signed-axis/16-bit-button report into the Xbox
 * model-1708 wire byte layout matching combined_report_map's gamepad
 * section: unsigned 16-bit axes (centre=32768), unsigned 10-bit triggers,
 * 4-bit hat, 15-bit button field. Host frameworks that pattern-match known
 * controller descriptors (e.g. GCExtendedGamepad) require this exact
 * layout — sending the internal struct's native layout produces stuck
 * axes and scrambled buttons on those hosts. */
static void gamepad_to_wire(const hid_gamepad_report_t *src, uint8_t dst[16])
{
    /* Convert signed 16-bit axes (-32768..32767) to unsigned (0..65535) */
    uint16_t ax[4];
    for (int i = 0; i < 4; i++) {
        ax[i] = (uint16_t)((int32_t)src->axes[i] + 32768);
    }

    /* Convert triggers: signed 16-bit → unsigned 10-bit (0..1023) */
    uint16_t lt = (uint16_t)(((int32_t)src->axes[4] + 32768) * 1023 / 65535);
    uint16_t rt = (uint16_t)(((int32_t)src->axes[5] + 32768) * 1023 / 65535);

    /* Map buttons: our 16-bit mask → 15-bit wire layout */
    uint32_t btn = 0;
    if (src->buttons & 0x0001) btn |= (1 << 0);  /* A */
    if (src->buttons & 0x0002) btn |= (1 << 1);  /* B */
    if (src->buttons & 0x0004) btn |= (1 << 2);  /* X */
    if (src->buttons & 0x0008) btn |= (1 << 3);  /* Y */
    if (src->buttons & 0x0010) btn |= (1 << 4);  /* LB */
    if (src->buttons & 0x0020) btn |= (1 << 5);  /* RB */
    if (src->buttons & 0x0040) btn |= (1 << 6);  /* Back/View */
    if (src->buttons & 0x0080) btn |= (1 << 7);  /* Start/Menu */
    if (src->buttons & 0x0100) btn |= (1 << 8);  /* Guide/Home */
    if (src->buttons & 0x0200) btn |= (1 << 9);  /* L3 (LS) */
    if (src->buttons & 0x0400) btn |= (1 << 10); /* R3 (RS) */

    /* Pack into wire format */
    dst[0]  = (uint8_t)(ax[0] & 0xFF);
    dst[1]  = (uint8_t)((ax[0] >> 8) & 0xFF);
    dst[2]  = (uint8_t)(ax[1] & 0xFF);
    dst[3]  = (uint8_t)((ax[1] >> 8) & 0xFF);
    dst[4]  = (uint8_t)(ax[2] & 0xFF);
    dst[5]  = (uint8_t)((ax[2] >> 8) & 0xFF);
    dst[6]  = (uint8_t)(ax[3] & 0xFF);
    dst[7]  = (uint8_t)((ax[3] >> 8) & 0xFF);
    dst[8]  = (uint8_t)(lt & 0xFF);
    dst[9]  = (uint8_t)((lt >> 8) & 0x03);
    dst[10] = (uint8_t)(rt & 0xFF);
    dst[11] = (uint8_t)((rt >> 8) & 0x03);
    dst[12] = (uint8_t)(src->hat & 0x0F);
    dst[13] = (uint8_t)(btn & 0xFF);
    dst[14] = (uint8_t)((btn >> 8) & 0x7F);
    dst[15] = 0; /* Share/Record button — no physical mapping */
}

static int ble_hid_send_gamepad(const hid_gamepad_report_t *report)
{
    if (!bt_hid_state.enabled) {
        return -EINVAL;
    }

    if (!bt_manager_is_connected()) {
        return -ENOTCONN;
    }

#if BT_AVAILABLE
    gamepad_to_wire(report, ble_gamepad_report);

    const struct bt_gatt_attr *attr = find_attr_by_user_data(ble_gamepad_report);
    if (!attr) {
        LOG_WRN("BLE HID: Gamepad attribute not found for notify");
        return -ENODEV;
    }

    int rc = bt_gatt_notify(NULL, attr, ble_gamepad_report,
                            sizeof(ble_gamepad_report));
    if (rc < 0) {
        LOG_ERR("BLE GP notify failed: %d", rc);
        return rc;
    }

    LOG_DBG("BLE GP notify sent: btns=%04x hat=%d", report->buttons, report->hat);
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

static int ble_hid_send_mouse(const hid_mouse_report_t *report)
{
    if (!bt_hid_state.enabled) {
        return -EINVAL;
    }

    if (!bt_manager_is_connected()) {
        return -ENOTCONN;
    }

#if BT_AVAILABLE
    memcpy(&current_mouse, report, sizeof(current_mouse));

    const struct bt_gatt_attr *attr = find_attr_by_user_data(&current_mouse);
    if (!attr) {
        LOG_WRN("BLE HID: Mouse attribute not found for notify");
        return -ENODEV;
    }

    int rc = bt_gatt_notify(NULL, attr, &current_mouse, sizeof(current_mouse));
    if (rc < 0) {
        LOG_ERR("BLE Mouse notify failed: %d", rc);
        return rc;
    }

    LOG_INF("BLE Mouse notify: btns=%02x dx=%d dy=%d wheel=%d",
            current_mouse.buttons, current_mouse.dx, current_mouse.dy,
            current_mouse.wheel);
#endif
    return 0;
}

static int ble_hid_send_consumer(const hid_consumer_report_t *report)
{
    if (!bt_hid_state.enabled) {
        return -EINVAL;
    }

    if (!bt_manager_is_connected()) {
        return -ENOTCONN;
    }

#if BT_AVAILABLE
    memcpy(&current_consumer, report, sizeof(current_consumer));

    const struct bt_gatt_attr *attr = find_attr_by_user_data(&current_consumer);
    if (!attr) {
        LOG_WRN("BLE HID: Consumer attribute not found for notify");
        return -ENODEV;
    }

    int rc = bt_gatt_notify(NULL, attr, &current_consumer, sizeof(current_consumer));
    if (rc < 0) {
        LOG_ERR("BLE Consumer notify failed: %d", rc);
        return rc;
    }

    LOG_INF("BLE Consumer notify: usage=0x%04x", current_consumer.usage);
#endif
    return 0;
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
    .send_mouse = ble_hid_send_mouse,
    .send_consumer = ble_hid_send_consumer,
    .register_event_cb = ble_hid_register_event_cb,
    .register_output_cb = ble_hid_register_output_cb,
    .is_connected = ble_hid_is_connected
};

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