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
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x01,        /*   Report ID (1) */
    
    /* Modifier keys */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0xE0,        /*   Usage Min (Left Control) */
    0x29, 0xE7,        /*   Usage Max (Right GUI) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x25, 0x01,        /*   Logical Max (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x08,        /*   Report Count (8) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute) */
    
    /* Reserved byte */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x01,        /*   Input (Constant) */
    
    /* LED output */
    0x05, 0x08,        /*   Usage Page (LEDs) */
    0x19, 0x01,        /*   Usage Min (Num Lock) */
    0x29, 0x05,        /*   Usage Max (Kana) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x05,        /*   Report Count (5) */
    0x91, 0x02,        /*   Output (Data, Variable, Absolute) */
    0x75, 0x03,        /*   Report Size (3) */
    0x95, 0x01,        /*   Report Count (1) */
    0x91, 0x01,        /*   Output (Constant) */
    
    /* Key array */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0x00,        /*   Usage Min (0) */
    0x29, 0xFF,        /*   Usage Max (255) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x26, 0xFF, 0x00,  /*   Logical Max (255) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x06,        /*   Report Count (6) */
    0x81, 0x00,        /*   Input (Data, Array) */
    
    0xC0               /* End Collection */
};

/* Gamepad Report Descriptor */
static const uint8_t gamepad_report_desc[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x05,        /* Usage (Game Pad) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x02,        /*   Report ID (2) */
    
    /* Axes */
    0x09, 0x30,        /*   Usage (X) */
    0x09, 0x31,        /*   Usage (Y) */
    0x09, 0x32,        /*   Usage (Z) - Right X */
    0x09, 0x35,        /*   Usage (Rz) - Right Y */
    0x16, 0x00, 0x80,  /*   Logical Min (-32768) */
    0x26, 0xFF, 0x7F,  /*   Logical Max (32767) */
    0x75, 0x10,        /*   Report Size (16) */
    0x95, 0x04,        /*   Report Count (4) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute) */
    
    /* Triggers */
    0x09, 0x33,        /*   Usage (Rx) - Left Trigger */
    0x09, 0x34,        /*   Usage (Ry) - Right Trigger */
    0x16, 0x00, 0x80,  /*   Logical Min (-32768) */
    0x26, 0xFF, 0x7F,  /*   Logical Max (32767) */
    0x75, 0x10,        /*   Report Size (16) */
    0x95, 0x02,        /*   Report Count (2) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute) */
    
    /* Buttons */
    0x05, 0x09,        /*   Usage Page (Buttons) */
    0x19, 0x01,        /*   Usage Min (1) */
    0x29, 0x10,        /*   Usage Max (16) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x25, 0x01,        /*   Logical Max (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x10,        /*   Report Count (16) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute) */
    
    /* Hat switch (D-pad) */
    0x05, 0x01,        /*   Usage Page (Generic Desktop) */
    0x09, 0x39,        /*   Usage (Hat Switch) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x25, 0x07,        /*   Logical Max (7) */
    0x35, 0x00,        /*   Physical Min (0) */
    0x46, 0x3B, 0x01,  /*   Physical Max (315) */
    0x65, 0x14,        /*   Unit (Degrees) */
    0x75, 0x04,        /*   Report Size (4) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x42,        /*   Input (Data, Variable, Null State) */
    0x75, 0x04,        /*   Report Size (4) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x01,        /*   Input (Constant) - padding */
    
    0xC0               /* End Collection */
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
/* BLE GATT Service (Stub - requires full implementation)                    */
/*===========================================================================*/

#if BT_AVAILABLE
/* TODO: Implement full GATT HID service with proper characteristics */
/* This requires registering HIDS, Battery Service, Device Info Service */
#endif

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
    if (!bt_hid_state.initialized) {
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
    if (!bt_hid_state.enabled) {
        return -EINVAL;
    }
    
    if (!bt_manager_is_connected()) {
        return -ENOTCONN;
    }
    
#if BT_AVAILABLE
    /* TODO: Send keyboard report via GATT notification */
    LOG_DBG("BLE KB: mod=%02x keys=[%02x %02x %02x %02x %02x %02x]",
            report->modifiers,
            report->keys[0], report->keys[1], report->keys[2],
            report->keys[3], report->keys[4], report->keys[5]);
#endif
    
    return 0;
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
    /* TODO: Send gamepad report via GATT notification */
    LOG_DBG("BLE GP: btns=%04x hat=%d", report->buttons, report->hat);
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
    .is_connected = ble_hid_is_connected
};

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int bt_hid_init(void)
{
    LOG_INF("Registering BLE HID transport");
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
