/**
 * @file usb_hid_device.c
 * @brief USB HID Device Implementation
 */

#include "usb_hid_device.h"
#include "usb_manager.h"
#include "../hid/hid_manager.h"
#include "../hid/hid_keyboard.h"
#include "../hid/hid_gamepad.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#if defined(CONFIG_USB_DEVICE_STACK)
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#define USB_AVAILABLE 1
#else
#define USB_AVAILABLE 0
#endif

LOG_MODULE_REGISTER(usb_hid_device, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* HID Report Descriptors                                                    */
/*===========================================================================*/

/* Combined Keyboard + Gamepad Report Descriptor */
static const uint8_t hid_report_desc[] = {
    /* Keyboard Report (Report ID 1) */
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

    0xC0, /* End Collection */

    /* Gamepad Report (Report ID 2) */
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
/* USB HID State                                                             */
/*===========================================================================*/

#if USB_AVAILABLE

static struct {
    bool initialized;
    bool configured;
    const struct device *hid_dev;
    
    usb_hid_output_callback_t output_cb;
    void *output_cb_user_data;
    
    struct k_mutex mutex;
} usb_hid_state;

/*===========================================================================*/
/* USB HID Callbacks                                                         */
/*===========================================================================*/

static void usb_hid_int_in_ready_cb(const struct device *dev)
{
    /* Interrupt IN endpoint ready - can send next report */
}

static void usb_hid_int_out_ready_cb(const struct device *dev)
{
    /* Interrupt OUT endpoint ready - host sent data */
    uint8_t buffer[64];
    uint32_t bytes_read;
    
    int ret = hid_int_ep_read(dev, buffer, sizeof(buffer), &bytes_read);
    if (ret == 0 && bytes_read > 0) {
        /* First byte is report ID, rest is data */
        uint8_t report_id = buffer[0];
        
        if (usb_hid_state.output_cb) {
            usb_hid_state.output_cb(report_id, &buffer[1], bytes_read - 1, 
                                   usb_hid_state.output_cb_user_data);
        }
        
        LOG_DBG("USB HID output report: ID=%u, len=%u", report_id, bytes_read - 1);
    }
}

static int usb_hid_get_report_cb(const struct device *dev,
                                  struct usb_setup_packet *setup,
                                  int32_t *len, uint8_t **data)
{
    uint8_t report_id = setup->wValue & 0xFF;
    uint8_t report_type = (setup->wValue >> 8) & 0xFF;
    
    LOG_DBG("GET_REPORT: type=%u, id=%u", report_type, report_id);
    
    /* Return empty report for now */
    *len = 0;
    return 0;
}

static int usb_hid_set_report_cb(const struct device *dev,
                                  struct usb_setup_packet *setup,
                                  int32_t *len, uint8_t **data)
{
    uint8_t report_id = setup->wValue & 0xFF;
    uint8_t report_type = (setup->wValue >> 8) & 0xFF;
    
    LOG_DBG("SET_REPORT: type=%u, id=%u, len=%d", report_type, report_id, *len);
    
    if (report_type == 0x02 && *len > 0) { /* Output report */
        if (usb_hid_state.output_cb) {
            usb_hid_state.output_cb(report_id, *data, *len, 
                                   usb_hid_state.output_cb_user_data);
        }
    }
    
    return 0;
}

static void usb_hid_status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
    switch (status) {
    case USB_DC_CONFIGURED:
        usb_hid_state.configured = true;
        LOG_INF("USB HID configured");
        break;
    case USB_DC_DISCONNECTED:
        usb_hid_state.configured = false;
        LOG_INF("USB HID disconnected");
        break;
    default:
        break;
    }
}

static const struct hid_ops usb_hid_ops = {
    .get_report = usb_hid_get_report_cb,
    .set_report = usb_hid_set_report_cb,
    .int_in_ready = usb_hid_int_in_ready_cb,
    .int_out_ready = usb_hid_int_out_ready_cb,
};

/*===========================================================================*/
/* Public API Implementation                                                 */
/*===========================================================================*/

int usb_hid_device_init(void)
{
    if (usb_hid_state.initialized) {
        return 0;
    }

    LOG_INF("Initializing USB HID device");

    k_mutex_init(&usb_hid_state.mutex);
    
    usb_hid_state.hid_dev = device_get_binding("HID_0");
    if (!usb_hid_state.hid_dev) {
        LOG_ERR("Failed to get HID device");
        return -ENODEV;
    }

    usb_hid_state.configured = false;
    usb_hid_state.output_cb = NULL;
    usb_hid_state.output_cb_user_data = NULL;
    usb_hid_state.initialized = true;

    return 0;
}

int usb_hid_device_deinit(void)
{
    if (!usb_hid_state.initialized) {
        return -EALREADY;
    }

    k_mutex_lock(&usb_hid_state.mutex, K_FOREVER);
    usb_hid_state.initialized = false;
    usb_hid_state.configured = false;
    k_mutex_unlock(&usb_hid_state.mutex);

    return 0;
}

int usb_hid_device_register(void)
{
    if (!usb_hid_state.initialized) {
        return -ENODEV;
    }

    /* Register HID report descriptor */
    usb_hid_register_device(usb_hid_state.hid_dev,
                           hid_report_desc, sizeof(hid_report_desc),
                           &usb_hid_ops);

    /* Register USB status callback */
    usb_enable(usb_hid_status_cb);

    LOG_INF("USB HID device registered");
    return 0;
}

int usb_hid_device_send_keyboard_report(const hid_keyboard_report_t *report)
{
    if (!usb_hid_state.initialized || !report) {
        return -EINVAL;
    }

    if (!usb_hid_state.configured) {
        return -ENOTCONN;
    }

    int ret = hid_int_ep_write(usb_hid_state.hid_dev, 
                               (uint8_t *)report, 
                               sizeof(hid_keyboard_report_t), 
                               NULL);
    
    if (ret < 0) {
        LOG_ERR("Failed to send keyboard report (err %d)", ret);
        return ret;
    }

    LOG_DBG("USB KB: mod=%02x keys=[%02x %02x %02x %02x %02x %02x]",
            report->modifiers,
            report->keys[0], report->keys[1], report->keys[2],
            report->keys[3], report->keys[4], report->keys[5]);

    return 0;
}

int usb_hid_device_send_gamepad_report(const hid_gamepad_report_t *report)
{
    if (!usb_hid_state.initialized || !report) {
        return -EINVAL;
    }

    if (!usb_hid_state.configured) {
        return -ENOTCONN;
    }

    int ret = hid_int_ep_write(usb_hid_state.hid_dev, 
                               (uint8_t *)report, 
                               sizeof(hid_gamepad_report_t), 
                               NULL);
    
    if (ret < 0) {
        LOG_ERR("Failed to send gamepad report (err %d)", ret);
        return ret;
    }

    LOG_DBG("USB GP: btns=%04x hat=%d", report->buttons, report->hat);

    return 0;
}

bool usb_hid_device_is_ready(void)
{
    return usb_hid_state.initialized && usb_hid_state.configured;
}

bool usb_hid_device_is_configured(void)
{
    return usb_hid_state.configured;
}

int usb_hid_device_register_output_callback(usb_hid_output_callback_t callback, 
                                             void *user_data)
{
    if (!usb_hid_state.initialized) {
        return -ENODEV;
    }

    k_mutex_lock(&usb_hid_state.mutex, K_FOREVER);
    usb_hid_state.output_cb = callback;
    usb_hid_state.output_cb_user_data = user_data;
    k_mutex_unlock(&usb_hid_state.mutex);

    return 0;
}

#else /* !USB_AVAILABLE */

/* Stub implementations when USB is disabled */
int usb_hid_device_init(void) { return -ENOTSUP; }
int usb_hid_device_deinit(void) { return -ENOTSUP; }
int usb_hid_device_register(void) { return -ENOTSUP; }
int usb_hid_device_send_keyboard_report(const hid_keyboard_report_t *r) { return -ENOTSUP; }
int usb_hid_device_send_gamepad_report(const hid_gamepad_report_t *r) { return -ENOTSUP; }
bool usb_hid_device_is_ready(void) { return false; }
bool usb_hid_device_is_configured(void) { return false; }
int usb_hid_device_register_output_callback(usb_hid_output_callback_t cb, void *ud) { return -ENOTSUP; }

#endif /* USB_AVAILABLE */
