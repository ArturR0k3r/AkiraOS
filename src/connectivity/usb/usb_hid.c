/**
 * @file usb_hid.c
 * @brief USB HID Transport Implementation for HID Manager
 * 
 * Implements USB HID keyboard transport using Zephyr 4.3 new USBD HID API.
 * Integrates with both USB Manager and HID Manager for complete USB HID support.
 */

#include "usb_hid.h"
#include "usb_manager.h"
#include "hid_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(usb_hid, CONFIG_LOG_DEFAULT_LEVEL);

/*===========================================================================*/
/* HID Report Descriptor - Boot Keyboard                                     */
/*===========================================================================*/

/**
 * @brief USB HID Boot Keyboard Report Descriptor
 * 
 * Standard boot keyboard protocol compatible with BIOS and all operating systems.
 * 
 * Report Structure (8 bytes):
 * - Byte 0: Modifier keys (Ctrl, Shift, Alt, GUI) - 8 bits
 * - Byte 1: Reserved (always 0)
 * - Bytes 2-7: Up to 6 simultaneous key codes
 */
static const uint8_t hid_kbd_report_desc[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    
    /* Modifier Byte - 8 bits for Ctrl/Shift/Alt/GUI */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0xE0,        /*   Usage Minimum (Left Ctrl) */
    0x29, 0xE7,        /*   Usage Maximum (Right GUI) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1 bit) */
    0x95, 0x08,        /*   Report Count (8 bits) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute) */
    
    /* Reserved Byte */
    0x75, 0x08,        /*   Report Size (8 bits) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x01,        /*   Input (Constant) */
    
    /* Key Array - 6 simultaneous keys */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0x00,        /*   Usage Minimum (0) */
    0x29, 0x65,        /*   Usage Maximum (101) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x65,        /*   Logical Maximum (101) */
    0x75, 0x08,        /*   Report Size (8 bits) */
    0x95, 0x06,        /*   Report Count (6) */
    0x81, 0x00,        /*   Input (Data, Array) */
    
    0xC0               /* End Collection */
};

/*===========================================================================*/
/* Constants                                                                  */
/*===========================================================================*/

#define USB_HID_KEYBOARD_REPORT_SIZE    8
#define USB_HID_PROTOCOL_BOOT           0
#define USB_HID_PROTOCOL_REPORT         1

/*===========================================================================*/
/* USB HID Context                                                           */
/*===========================================================================*/

/**
 * @brief USB HID transport context
 */
static struct {
    const struct device *hid_dev;        /* HID device from device tree */
    bool initialized;                     /* Transport initialized */
    bool interface_ready;                 /* USB interface ready for reports */
    bool enabled;                         /* Transport enabled */
    uint8_t protocol;                     /* Current protocol (boot/report) */
    uint8_t idle_rate;                    /* Current idle rate */
    struct k_mutex mutex;                 /* Thread safety */
    struct k_sem report_sem;              /* Report completion semaphore */
} usb_hid_ctx = {
    .initialized = false,
    .interface_ready = false,
    .enabled = false,
    .protocol = USB_HID_PROTOCOL_REPORT,
    .idle_rate = 0,
};

/*===========================================================================*/
/* Forward Declarations                                                       */
/*===========================================================================*/

static int usb_hid_transport_init_fn(hid_device_type_t device_types);
static int usb_hid_transport_enable(void);
static int usb_hid_transport_disable(void);
static bool usb_hid_transport_is_connected(void);
static int usb_hid_transport_send_keyboard(const hid_keyboard_report_t *report);
static int usb_hid_transport_send_gamepad(const hid_gamepad_report_t *report);

/*===========================================================================*/
/* HID Device Callbacks                                                      */
/*===========================================================================*/

/**
 * @brief Interface ready callback
 * 
 * Called when the HID interface becomes active or inactive.
 */
static void usb_hid_iface_ready(const struct device *dev, const bool ready)
{
    ARG_UNUSED(dev);
    
    LOG_INF("HID interface %s", ready ? "ready" : "not ready");
    
    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    usb_hid_ctx.interface_ready = ready;
    if (ready) {
        usb_hid_ctx.enabled = true;
    }
    k_mutex_unlock(&usb_hid_ctx.mutex);
}

/**
 * @brief Get HID report
 * 
 * Called by USB stack when host requests a report (GET_REPORT).
 */
static int usb_hid_get_report(const struct device *dev,
                              const uint8_t type, const uint8_t id,
                              const uint16_t len, uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);
    
    if (type != HID_REPORT_TYPE_INPUT) {
        return -ENOTSUP;
    }
    
    if (len < USB_HID_KEYBOARD_REPORT_SIZE) {
        return -ENOBUFS;
    }
    
    /* Return current keyboard state - all zeros for now */
    memset(buf, 0, USB_HID_KEYBOARD_REPORT_SIZE);
    
    return USB_HID_KEYBOARD_REPORT_SIZE;
}

/**
 * @brief Set HID report
 * 
 * Called by USB stack when host sends a report (SET_REPORT).
 * This is a required callback but we don't handle output reports.
 */
static int usb_hid_set_report(const struct device *dev,
                              const uint8_t type, const uint8_t id,
                              const uint16_t len, const uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);
    
    /* Accept but ignore all output reports */
    return 0;
}

/**
 * @brief Protocol change callback
 * 
 * Called by USB stack when host changes protocol (boot vs report).
 */
static void usb_hid_set_protocol(const struct device *dev, const uint8_t proto)
{
    ARG_UNUSED(dev);
    
    LOG_INF("Protocol changed to: %s", 
            proto == USB_HID_PROTOCOL_BOOT ? "Boot" : "Report");
    
    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    usb_hid_ctx.protocol = proto;
    k_mutex_unlock(&usb_hid_ctx.mutex);
}

/**
 * @brief Idle callback
 * 
 * Called by USB stack when idle rate changes.
 */
static void usb_hid_set_idle(const struct device *dev,
                             const uint8_t id, const uint32_t duration)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);

    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    usb_hid_ctx.idle_rate = duration;
    k_mutex_unlock(&usb_hid_ctx.mutex);
}

/**
 * @brief Get idle rate
 */
static uint32_t usb_hid_get_idle(const struct device *dev, const uint8_t id)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);
    
    uint32_t idle;
    
    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    idle = usb_hid_ctx.idle_rate;
    k_mutex_unlock(&usb_hid_ctx.mutex);
    
    return idle;
}

/**
 * @brief Input report done callback
 * 
 * Called when an input report has been successfully sent.
 */
static void usb_hid_input_report_done(const struct device *dev,
                                      const uint8_t *const report)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(report);
    
    /* Signal that we can send another report */
    k_sem_give(&usb_hid_ctx.report_sem);
}

/* HID device operations structure */
static const struct hid_device_ops usb_hid_ops = {
    .iface_ready = usb_hid_iface_ready,
    .get_report = usb_hid_get_report,
    .set_report = usb_hid_set_report,
    .set_idle = usb_hid_set_idle,
    .get_idle = usb_hid_get_idle,
    .set_protocol = usb_hid_set_protocol,
    .input_report_done = usb_hid_input_report_done,
};

/*===========================================================================*/
/* Transport Implementation                                                  */
/*===========================================================================*/

static char* type_to_str(hid_device_type_t type)
{
    switch (type) {
        case HID_DEVICE_KEYBOARD:
            return "Keyboard";
        case HID_DEVICE_GAMEPAD:
            return "Gamepad";
        case HID_DEVICE_MOUSE:
            return "Mouse";
        case HID_DEVICE_COMBO:
            return "Combo";
        default:
            return "Unknown";
    }
}

/**
 * @brief Initialize USB HID transport
 * 
 * @param device_types HID device types to support
 * @return 0 on success, negative error code on failure
 */
static int usb_hid_transport_init_fn(hid_device_type_t device_types)
{
    int ret;
    struct usbd_context *usbd_ctx;
    
    if (usb_hid_ctx.initialized) {
        LOG_WRN("USB HID transport already initialized");
        return 0;
    }
    
    LOG_INF("Initializing USB HID transport (%s)", type_to_str(device_types));
    
    /* Validate device type - only keyboard supported by Zephyr USBD stack*/
    if (!(device_types & HID_DEVICE_KEYBOARD)) {
        LOG_ERR("Only keyboard device type is currently supported");
        return -ENOTSUP;
    }
    
    /* Initialize synchronization primitives */
    ret = k_mutex_init(&usb_hid_ctx.mutex);
    if (ret) {
        LOG_ERR("Failed to initialize mutex: %d", ret);
        return ret;
    }
    
    ret = k_sem_init(&usb_hid_ctx.report_sem, 1, 1);
    if (ret) {
        LOG_ERR("Failed to initialize semaphore: %d", ret);
        return ret;
    }
    
    /* USB manager needs to initialized to get context */
    ret = usb_manager_is_initialized();
    if (!ret) {
        LOG_ERR("USB manager must be initialized before HID transport");
        return -EAGAIN;
    }

    usbd_ctx = usb_manager_get_context();
    if (usbd_ctx == NULL) {
        LOG_ERR("Failed to get USB device context");
        return -ENODEV;
    }
    
    /* Get HID device from device tree */
    usb_hid_ctx.hid_dev = DEVICE_DT_GET(DT_NODELABEL(hid_dev_0));
    if (!device_is_ready(usb_hid_ctx.hid_dev)) {
        LOG_ERR("HID device not ready");
        return -ENODEV;
    }
    
    /* Register HID device with report descriptor and callbacks */
    ret = hid_device_register(usb_hid_ctx.hid_dev,
                              hid_kbd_report_desc,
                              sizeof(hid_kbd_report_desc),
                              &usb_hid_ops);
    if (ret) {
        LOG_ERR("Failed to register HID device: %d", ret);
        return ret;
    }

    /* Register ALL classes  */
    ret = usbd_register_all_classes(usbd_ctx, USBD_SPEED_FS, 1, NULL);
    if (ret) {
        LOG_ERR("Failed to register USB classes: %d", ret);
        return ret;
    }

    /* Finalize USB stack (calls usbd_init()) */
    ret = usb_manager_finalize();
    if (ret) {
        LOG_ERR("Failed to finalize USB manager: %d", ret);
        return ret;
    }

    usb_hid_ctx.initialized = true;
    
    LOG_INF("USB HID transport initialized successfully");
    
    return 0;
}

/**
 * @brief Enable USB HID transport
 * 
 * @return 0 on success, negative error code on failure
 */
static int usb_hid_transport_enable(void)
{
    int ret;
    
    if (!usb_hid_ctx.initialized) {
        LOG_ERR("USB HID transport not initialized");
        return -EINVAL;
    }
    
    if (usb_hid_ctx.enabled) {
        LOG_WRN("USB HID transport already enabled");
        return 0;
    }
    
    LOG_INF("Enabling USB HID transport");
    
    /* Enable USB device via USB manager */
    ret = usb_manager_enable();
    if (ret && ret != -EALREADY) {
        LOG_ERR("Failed to enable USB manager: %d", ret);
        return ret;
    }
    
    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    usb_hid_ctx.enabled = true;
    k_mutex_unlock(&usb_hid_ctx.mutex);
    
    LOG_INF("USB HID transport enabled successfully");
    
    return 0;
}

/**
 * @brief Disable USB HID transport
 * 
 * @return 0 on success, negative error code on failure
 */
static int usb_hid_transport_disable(void)
{
    if (!usb_hid_ctx.initialized) {
        return 0;
    }
    
    LOG_INF("Disabling USB HID transport");
    
    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    usb_hid_ctx.enabled = false;
    usb_hid_ctx.interface_ready = false;
    k_mutex_unlock(&usb_hid_ctx.mutex);
    
    LOG_INF("USB HID transport disabled");
    
    return 0;
}

/**
 * @brief Check if USB HID is connected
 * 
 * @return true if connected and ready to send reports, false otherwise
 */
static bool usb_hid_transport_is_connected(void)
{
    return usb_hid_ctx.enabled && usb_hid_ctx.interface_ready;
}

/**
 * @brief Send keyboard report via USB HID
 * 
 * @param report Pointer to keyboard report structure
 * @return 0 on success, negative error code on failure
 */
static int usb_hid_transport_send_keyboard(const hid_keyboard_report_t *report)
{
    int ret;
    uint8_t report_buf[USB_HID_KEYBOARD_REPORT_SIZE];
    
    if (!usb_hid_ctx.initialized || !report) {
        return -EINVAL;
    }
    
    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    
    /* Check if we can send reports */
    if (!usb_hid_ctx.enabled) {
        k_mutex_unlock(&usb_hid_ctx.mutex);
        return -EAGAIN;
    }
    
    if (!usb_hid_ctx.interface_ready) {
        k_mutex_unlock(&usb_hid_ctx.mutex);
        return -EAGAIN;
    }
    
    /* Build HID keyboard report */
    report_buf[0] = report->modifiers;      /* Modifier byte */
    report_buf[1] = 0;                      /* Reserved byte */
    memcpy(&report_buf[2], report->keys, HID_MAX_KEYS);  /* Key array (6 bytes) */
    
    k_mutex_unlock(&usb_hid_ctx.mutex);
    
    /* Wait for previous report to complete (non-blocking with timeout) */
    ret = k_sem_take(&usb_hid_ctx.report_sem, K_MSEC(100));
    if (ret) {
        LOG_WRN("Timeout waiting for previous report to complete");
        return -EBUSY;
    }
    
    /* Send report via USB HID */
    ret = hid_device_submit_report(usb_hid_ctx.hid_dev, 
                                   USB_HID_KEYBOARD_REPORT_SIZE,
                                   report_buf);
    
    if (ret) {
        k_sem_give(&usb_hid_ctx.report_sem);
        LOG_ERR("Failed to send keyboard report: %d", ret);
        return ret;
    }
    
    LOG_INF("Sent keyboard report: mod=0x%02x keys=[%02x %02x %02x %02x %02x %02x]",
            report->modifiers,
            report->keys[0], report->keys[1], report->keys[2],
            report->keys[3], report->keys[4], report->keys[5]);
    
    /* Semaphore will be released in input_report_done callback */
    
    return 0;
}

/**
 * @brief Send gamepad report via USB HID
 * 
 * @note Currently not implemented - keyboard only
 */
static int usb_hid_transport_send_gamepad(const hid_gamepad_report_t *report)
{
    ARG_UNUSED(report);
    
    LOG_WRN("Gamepad not supported in Zephyr USB HID stack");
    return -ENOTSUP;
}

/* Transport operations structure for HID manager */
static const hid_transport_ops_t usb_hid_transport_ops = {
    .name = "usb",
    .init = usb_hid_transport_init_fn,
    .enable = usb_hid_transport_enable,
    .disable = usb_hid_transport_disable,
    .is_connected = usb_hid_transport_is_connected,
    .send_keyboard = usb_hid_transport_send_keyboard,
    .send_gamepad = usb_hid_transport_send_gamepad,
};

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

/**
 * @brief Initialize and register USB HID transport
 * 
 * This function should be called during system initialization to register
 * the USB HID transport with the HID manager.
 * 
 * @return 0 on success, negative error code on failure
 */
int usb_hid_transport_init(void)
{
    int ret;
    
    LOG_INF("Registering USB HID transport with HID manager");
    
    /* Register transport with HID manager */
    ret = hid_manager_register_transport(&usb_hid_transport_ops);
    if (ret) {
        LOG_ERR("Failed to register USB HID transport: %d", ret);
        return ret;
    }
    
    LOG_INF("USB HID transport registered successfully");
    
    return 0;
}

/**
 * @brief Get the USB HID device
 * 
 * @return Pointer to HID device, or NULL if not initialized
 */
const struct device *usb_hid_get_device(void)
{
    if (!usb_hid_ctx.initialized) {
        return NULL;
    }
    
    return usb_hid_ctx.hid_dev;
}

/**
 * @brief Check if USB HID is ready to send reports
 * 
 * @return true if ready, false otherwise
 */
bool usb_hid_is_ready(void)
{
    return usb_hid_transport_is_connected();
}

/**
 * @brief Get current protocol
 * 
 * @return Current protocol (0=boot, 1=report)
 */
uint8_t usb_hid_get_protocol(void)
{
    return usb_hid_ctx.protocol;
}
