/**
 * @file usb_hid.c
 * @brief USB HID Transport Layer for AkiraOS
 */

#include "usb_hid.h"
#include "usb_hid_device.h"
#include "usb_manager.h"
#include "../hid/hid_manager.h"
#include "../hid/hid_keyboard.h"
#include "../hid/hid_gamepad.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(usb_hid, CONFIG_AKIRA_LOG_LEVEL);

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
} usb_hid_state;

/*===========================================================================*/
/* USB HID Device Output Callback                                            */
/*===========================================================================*/

static void usb_hid_output_handler(uint8_t report_id, const uint8_t *data, 
                                    uint16_t len, void *user_data)
{
    /* Forward output reports to registered callback */
    if (usb_hid_state.output_cb) {
        usb_hid_state.output_cb(data, len, usb_hid_state.output_cb_data);
    }
    
    LOG_DBG("USB HID output: ID=%u, len=%u", report_id, len);
}

/*===========================================================================*/
/* Transport Implementation                                                  */
/*===========================================================================*/

static int usb_hid_transport_init(hid_device_type_t types)
{
    int ret;
    
    LOG_INF("USB HID transport init (types=0x%02x)", types);

    usb_hid_state.device_types = types;

    /* Initialize USB HID device layer */
    ret = usb_hid_device_init();
    if (ret < 0) {
        LOG_ERR("USB HID device init failed (err %d)", ret);
        return ret;
    }

    /* Register output callback */
    ret = usb_hid_device_register_output_callback(usb_hid_output_handler, NULL);
    if (ret < 0) {
        LOG_WRN("Failed to register output callback (err %d)", ret);
    }

    /* Register USB HID device with USB stack */
    ret = usb_hid_device_register();
    if (ret < 0) {
        LOG_ERR("USB HID device register failed (err %d)", ret);
        usb_hid_device_deinit();
        return ret;
    }

    usb_hid_state.initialized = true;
    return 0;
}

static int usb_hid_transport_deinit(void)
{
    usb_hid_device_deinit();
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
    LOG_INF("USB HID transport enabled");
    return 0;
}

static int usb_hid_transport_disable(void)
{
    usb_hid_state.enabled = false;
    LOG_INF("USB HID transport disabled");
    return 0;
}

static int usb_hid_send_keyboard(const hid_keyboard_report_t *report)
{
    if (!usb_hid_state.enabled) {
        return -EINVAL;
    }

    return usb_hid_device_send_keyboard_report(report);
}

static int usb_hid_send_gamepad(const hid_gamepad_report_t *report)
{
    if (!usb_hid_state.enabled) {
        return -EINVAL;
    }

    return usb_hid_device_send_gamepad_report(report);
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
    return usb_hid_device_is_configured();
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
    .is_connected = usb_hid_is_connected};

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
