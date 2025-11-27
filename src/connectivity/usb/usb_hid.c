/**
 * @file usb_hid.c
 * @brief USB HID Device Implementation for AkiraOS
 */

#include "usb_hid.h"
#include "usb_manager.h"
#include "../hid/hid_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#if defined(CONFIG_USB_DEVICE_HID)
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#define USB_HID_AVAILABLE 1
#else
#define USB_HID_AVAILABLE 0
#endif

LOG_MODULE_REGISTER(usb_hid, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* HID Report Descriptors                                                    */
/*===========================================================================*/

/* Combined Keyboard + Gamepad Report Descriptor */
static const uint8_t hid_report_desc[] = {
    /* Keyboard */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x01,        /*   Report ID (1) */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0xE0,        /*   Usage Min (Left Control) */
    0x29, 0xE7,        /*   Usage Max (Right GUI) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x25, 0x01,        /*   Logical Max (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x08,        /*   Report Count (8) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x01,        /*   Input (Constant) */
    0x05, 0x08,        /*   Usage Page (LEDs) */
    0x19, 0x01,        /*   Usage Min (Num Lock) */
    0x29, 0x05,        /*   Usage Max (Kana) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x05,        /*   Report Count (5) */
    0x91, 0x02,        /*   Output (Data, Variable, Absolute) */
    0x75, 0x03,        /*   Report Size (3) */
    0x95, 0x01,        /*   Report Count (1) */
    0x91, 0x01,        /*   Output (Constant) */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0x00,        /*   Usage Min (0) */
    0x29, 0xFF,        /*   Usage Max (255) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x26, 0xFF, 0x00,  /*   Logical Max (255) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x06,        /*   Report Count (6) */
    0x81, 0x00,        /*   Input (Data, Array) */
    0xC0,              /* End Collection */
    
    /* Gamepad */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x05,        /* Usage (Game Pad) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x02,        /*   Report ID (2) */
    0x09, 0x30,        /*   Usage (X) */
    0x09, 0x31,        /*   Usage (Y) */
    0x09, 0x32,        /*   Usage (Z) */
    0x09, 0x35,        /*   Usage (Rz) */
    0x16, 0x00, 0x80,  /*   Logical Min (-32768) */
    0x26, 0xFF, 0x7F,  /*   Logical Max (32767) */
    0x75, 0x10,        /*   Report Size (16) */
    0x95, 0x04,        /*   Report Count (4) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute) */
    0x09, 0x33,        /*   Usage (Rx) - LT */
    0x09, 0x34,        /*   Usage (Ry) - RT */
    0x16, 0x00, 0x80,  /*   Logical Min */
    0x26, 0xFF, 0x7F,  /*   Logical Max */
    0x75, 0x10,        /*   Report Size (16) */
    0x95, 0x02,        /*   Report Count (2) */
    0x81, 0x02,        /*   Input */
    0x05, 0x09,        /*   Usage Page (Buttons) */
    0x19, 0x01,        /*   Usage Min (1) */
    0x29, 0x10,        /*   Usage Max (16) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x25, 0x01,        /*   Logical Max (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x10,        /*   Report Count (16) */
    0x81, 0x02,        /*   Input */
    0x05, 0x01,        /*   Usage Page (Generic Desktop) */
    0x09, 0x39,        /*   Usage (Hat Switch) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x25, 0x07,        /*   Logical Max (7) */
    0x75, 0x04,        /*   Report Size (4) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x42,        /*   Input (Null State) */
    0x75, 0x04,        /*   Report Size (4) - padding */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x01,        /*   Input (Constant) */
    0xC0               /* End Collection */
};

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct {
    bool initialized;
    bool enabled;
    hid_device_type_t device_types;
    
#if USB_HID_AVAILABLE
    const struct device *hid_dev;
#endif
    
    hid_event_callback_t event_cb;
    void *event_cb_data;
    hid_output_callback_t output_cb;
    void *output_cb_data;
} usb_hid_state;

/*===========================================================================*/
/* USB HID Callbacks                                                         */
/*===========================================================================*/

#if USB_HID_AVAILABLE
static void hid_int_ready_cb(const struct device *dev)
{
    /* Interrupt IN endpoint is ready for next report */
}

static void hid_out_ready_cb(const struct device *dev, uint8_t report_id)
{
    /* Output report received from host (e.g., LED state) */
    if (usb_hid_state.output_cb) {
        uint8_t data[8];
        int ret = hid_int_ep_read(dev, data, sizeof(data), NULL);
        if (ret >= 0) {
            usb_hid_state.output_cb(data, ret, usb_hid_state.output_cb_data);
        }
    }
}

static const struct hid_ops hid_ops = {
    .int_in_ready = hid_int_ready_cb,
    .int_out_ready = hid_out_ready_cb,
};
#endif

/*===========================================================================*/
/* Transport Implementation                                                  */
/*===========================================================================*/

static int usb_hid_transport_init(hid_device_type_t types)
{
    LOG_INF("USB HID init (types=0x%02x)", types);
    
    usb_hid_state.device_types = types;
    
#if USB_HID_AVAILABLE
    usb_hid_state.hid_dev = device_get_binding("HID_0");
    if (!usb_hid_state.hid_dev) {
        LOG_ERR("USB HID device not found");
        return -ENODEV;
    }
    
    usb_hid_register_device(usb_hid_state.hid_dev,
                            hid_report_desc,
                            sizeof(hid_report_desc),
                            &hid_ops);
    
    usb_hid_init(usb_hid_state.hid_dev);
#endif
    
    usb_hid_state.initialized = true;
    return 0;
}

static int usb_hid_transport_deinit(void)
{
    usb_hid_state.initialized = false;
    usb_hid_state.enabled = false;
    return 0;
}

static int usb_hid_transport_enable(void)
{
    if (!usb_hid_state.initialized) {
        return -EINVAL;
    }
    
    usb_hid_state.enabled = true;
    return usb_manager_enable();
}

static int usb_hid_transport_disable(void)
{
    usb_hid_state.enabled = false;
    return 0;
}

static int usb_hid_send_keyboard(const hid_keyboard_report_t *report)
{
    if (!usb_hid_state.enabled) {
        return -EINVAL;
    }
    
    if (!usb_manager_is_connected()) {
        return -ENOTCONN;
    }
    
#if USB_HID_AVAILABLE
    uint8_t buf[9];
    buf[0] = 0x01;  /* Report ID */
    buf[1] = report->modifiers;
    buf[2] = report->reserved;
    memcpy(&buf[3], report->keys, 6);
    
    int ret = hid_int_ep_write(usb_hid_state.hid_dev, buf, sizeof(buf), NULL);
    if (ret < 0) {
        LOG_ERR("USB HID keyboard write failed: %d", ret);
        return ret;
    }
#else
    LOG_DBG("USB KB: mod=%02x keys=[%02x %02x %02x %02x %02x %02x]",
            report->modifiers,
            report->keys[0], report->keys[1], report->keys[2],
            report->keys[3], report->keys[4], report->keys[5]);
#endif
    
    return 0;
}

static int usb_hid_send_gamepad(const hid_gamepad_report_t *report)
{
    if (!usb_hid_state.enabled) {
        return -EINVAL;
    }
    
    if (!usb_manager_is_connected()) {
        return -ENOTCONN;
    }
    
#if USB_HID_AVAILABLE
    uint8_t buf[16];
    buf[0] = 0x02;  /* Report ID */
    memcpy(&buf[1], report->axes, 12);  /* 6 axes * 2 bytes */
    buf[13] = report->buttons & 0xFF;
    buf[14] = (report->buttons >> 8) & 0xFF;
    buf[15] = report->hat;
    
    int ret = hid_int_ep_write(usb_hid_state.hid_dev, buf, sizeof(buf), NULL);
    if (ret < 0) {
        LOG_ERR("USB HID gamepad write failed: %d", ret);
        return ret;
    }
#else
    LOG_DBG("USB GP: btns=%04x hat=%d", report->buttons, report->hat);
#endif
    
    return 0;
}

static int usb_hid_register_event_cb(hid_event_callback_t cb, void *user_data)
{
    usb_hid_state.event_cb = cb;
    usb_hid_state.event_cb_data = user_data;
    return 0;
}

static int usb_hid_register_output_cb(hid_output_callback_t cb, void *user_data)
{
    usb_hid_state.output_cb = cb;
    usb_hid_state.output_cb_data = user_data;
    return 0;
}

static bool usb_hid_is_connected(void)
{
    return usb_manager_is_connected();
}

/*===========================================================================*/
/* Transport Operations Structure                                            */
/*===========================================================================*/

static const hid_transport_ops_t usb_hid_transport = {
    .name = "usb",
    .init = usb_hid_transport_init,
    .deinit = usb_hid_transport_deinit,
    .enable = usb_hid_transport_enable,
    .disable = usb_hid_transport_disable,
    .send_keyboard = usb_hid_send_keyboard,
    .send_gamepad = usb_hid_send_gamepad,
    .register_event_cb = usb_hid_register_event_cb,
    .register_output_cb = usb_hid_register_output_cb,
    .is_connected = usb_hid_is_connected
};

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int usb_hid_init(void)
{
    LOG_INF("Registering USB HID transport");
    return hid_manager_register_transport(&usb_hid_transport);
}

const hid_transport_ops_t *usb_hid_get_transport(void)
{
    return &usb_hid_transport;
}

int usb_hid_enable(void)
{
    return usb_hid_transport_enable();
}

int usb_hid_disable(void)
{
    return usb_hid_transport_disable();
}
