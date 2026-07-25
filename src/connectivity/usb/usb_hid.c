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
static const uint8_t hid_report_desc[] = {
    /* Keyboard — Report ID 1 */
    0x05,
    0x01,
    0x09,
    0x06,
    0xA1,
    0x01,
    0x85,
    0x01, /* Report ID (1) */
    0x05,
    0x07,
    0x19,
    0xE0,
    0x29,
    0xE7,
    0x15,
    0x00,
    0x25,
    0x01,
    0x75,
    0x01,
    0x95,
    0x08,
    0x81,
    0x02, /* Modifiers */
    0x75,
    0x08,
    0x95,
    0x01,
    0x81,
    0x01, /* Reserved */
    0x05,
    0x08,
    0x19,
    0x01,
    0x29,
    0x05, /* LED output */
    0x75,
    0x01,
    0x95,
    0x05,
    0x91,
    0x02,
    0x75,
    0x03,
    0x95,
    0x01,
    0x91,
    0x01,
    0x05,
    0x07,
    0x19,
    0x00,
    0x29,
    0x65,
    0x15,
    0x00,
    0x25,
    0x65,
    0x75,
    0x08,
    0x95,
    0x06,
    0x81,
    0x00, /* Keys */
    0xC0,

    /* Mouse — Report ID 2 */
    0x05,
    0x01,
    0x09,
    0x02,
    0xA1,
    0x01,
    0x85,
    0x02, /* Report ID (2) */
    0x09,
    0x01,
    0xA1,
    0x00,
    0x05,
    0x09,
    0x19,
    0x01,
    0x29,
    0x05, /* 5 buttons */
    0x15,
    0x00,
    0x25,
    0x01,
    0x75,
    0x01,
    0x95,
    0x05,
    0x81,
    0x02,
    0x75,
    0x03,
    0x95,
    0x01,
    0x81,
    0x01, /* padding */
    0x05,
    0x01,
    0x09,
    0x30,
    0x09,
    0x31, /* X, Y */
    0x15,
    0x81,
    0x25,
    0x7F,
    0x75,
    0x08,
    0x95,
    0x02,
    0x81,
    0x06,
    0x09,
    0x38, /* Wheel */
    0x15,
    0x81,
    0x25,
    0x7F,
    0x75,
    0x08,
    0x95,
    0x01,
    0x81,
    0x06,
    0xC0,
    0xC0,

    /* Vendor — Report ID 3 (AkiraOS 64-byte raw HID channel) */
    /* Usage Page 0xFF60 / Usage 0x61 matches QMK RAW HID convention */
    0x06,
    0x60,
    0xFF, /* Usage Page (Vendor 0xFF60) */
    0x09,
    0x61, /* Usage (0x61) */
    0xA1,
    0x01, /* Collection (Application) */
    0x85,
    0x03, /* Report ID (3) */
    /* IN: device → host (responses / notifications) */
    0x09,
    0x62, /* Usage (0x62) */
    0x15,
    0x00, /* Logical Minimum (0) */
    0x26,
    0xFF,
    0x00, /* Logical Maximum (255) */
    0x75,
    0x08, /* Report Size (8 bits) */
    0x95,
    0x3F, /* Report Count (63 bytes) */
    0x81,
    0x02, /* Input (Data, Var, Abs) */
    /* OUT: host → device (commands) */
    0x09,
    0x63, /* Usage (0x63) */
    0x15,
    0x00, /* Logical Minimum (0) */
    0x26,
    0xFF,
    0x00, /* Logical Maximum (255) */
    0x75,
    0x08, /* Report Size (8 bits) */
    0x95,
    0x3F, /* Report Count (63 bytes) */
    0x91,
    0x02, /* Output (Data, Var, Abs) */
    0xC0, /* End Collection */
};

/**
 * @brief FIDO2/CTAP2 authenticator report descriptor — dedicated interface
 *
 * No Report ID: CTAPHID requires the wire packet to be exactly 64 bytes,
 * which would overflow the 64-byte full-speed interrupt endpoint cap if a
 * Report ID byte were prepended (see hid_dev_1 in the board overlay).
 * Usage Page 0xF1D0 / Usage 0x01 per FIDO HID Protocol Specification.
 */
static const uint8_t fido_report_desc[] = {
    0x06,
    0xD0,
    0xF1, /* Usage Page (FIDO Alliance 0xF1D0) */
    0x09,
    0x01, /* Usage (U2FHID Authenticator Device) */
    0xA1,
    0x01, /* Collection (Application) */
    /* IN: device → host */
    0x09,
    0x20, /* Usage (Input Report Data) */
    0x15,
    0x00, /* Logical Minimum (0) */
    0x26,
    0xFF,
    0x00, /* Logical Maximum (255) */
    0x75,
    0x08, /* Report Size (8 bits) */
    0x95,
    0x40, /* Report Count (64 bytes) */
    0x81,
    0x02, /* Input (Data, Var, Abs) */
    /* OUT: host → device */
    0x09,
    0x21, /* Usage (Output Report Data) */
    0x15,
    0x00, /* Logical Minimum (0) */
    0x26,
    0xFF,
    0x00, /* Logical Maximum (255) */
    0x75,
    0x08, /* Report Size (8 bits) */
    0x95,
    0x40, /* Report Count (64 bytes) */
    0x91,
    0x02, /* Output (Data, Var, Abs) */
    0xC0, /* End Collection */
};

#if defined(CONFIG_AKIRA_HID_GAMEPAD)
/*===========================================================================*/
/* HID Report Descriptor - Gamepad                                           */
/*===========================================================================*/

/**
 * @brief USB HID Gamepad Report Descriptor
 *
 * Kept in its own descriptor (not merged into hid_report_desc) because
 * combining a gamepad collection with the keyboard/mouse collections
 * breaks iOS HID enumeration. AKIRA_HID_MODE selects exactly one of the
 * two descriptors at build time.
 */
static const uint8_t hid_gamepad_report_desc[] = {
    /* Gamepad — Report ID 1 */
    0x05,
    0x01, /* Usage Page (Generic Desktop) */
    0x09,
    0x05, /* Usage (Gamepad) */
    0xA1,
    0x01, /* Collection (Application) */
    0x85,
    0x01, /* Report ID (1) */
    0x09,
    0x30, /* Usage (X) */
    0x09,
    0x31, /* Usage (Y) */
    0x09,
    0x32, /* Usage (Z) */
    0x09,
    0x33, /* Usage (Rx) */
    0x09,
    0x34, /* Usage (Ry) */
    0x09,
    0x35, /* Usage (Rz) */
    0x16,
    0x00,
    0x80, /* Logical Minimum (-32768) */
    0x26,
    0xFF,
    0x7F, /* Logical Maximum (32767) */
    0x75,
    0x10, /* Report Size (16) */
    0x95,
    0x06, /* Report Count (6 axes) */
    0x81,
    0x02, /* Input (Data, Var, Abs) */
    0x05,
    0x09, /* Usage Page (Button) */
    0x19,
    0x01, /* Usage Minimum (1) */
    0x29,
    0x10, /* Usage Maximum (16) */
    0x15,
    0x00, /* Logical Minimum (0) */
    0x25,
    0x01, /* Logical Maximum (1) */
    0x75,
    0x01, /* Report Size (1) */
    0x95,
    0x10, /* Report Count (16 buttons) */
    0x81,
    0x02, /* Input (Data, Var, Abs) */
    0x05,
    0x01, /* Usage Page (Generic Desktop) */
    0x09,
    0x39, /* Usage (Hat Switch) */
    0x15,
    0x00, /* Logical Minimum (0) */
    0x25,
    0x08, /* Logical Maximum (8) */
    0x75,
    0x08, /* Report Size (8) */
    0x95,
    0x01, /* Report Count (1) */
    0x81,
    0x02, /* Input (Data, Var, Abs) */
    0x75,
    0x08, /* Report Size (8) */
    0x95,
    0x01, /* Report Count (1) */
    0x81,
    0x01, /* Input (Const) — reserved byte */
    0xC0, /* End Collection */
};
#endif /* CONFIG_AKIRA_HID_GAMEPAD */

/*===========================================================================*/
/* Constants                                                                  */
/*===========================================================================*/

#define USB_HID_KEYBOARD_REPORT_SIZE 9
#define USB_HID_MOUSE_REPORT_SIZE 5
#define USB_HID_RAW_REPORT_SIZE 64 /* Report ID (1) + 63-byte payload */
#define USB_HID_RAW_PAYLOAD_SIZE 63
#define USB_HID_RAW_REPORT_ID 3
#define USB_HID_FIDO_REPORT_SIZE 64 /* No Report ID — dedicated interface */
#define USB_HID_FIDO_PAYLOAD_SIZE 64
#define USB_HID_GAMEPAD_REPORT_SIZE 17 /* Report ID (1) + 16-byte hid_gamepad_report_t */
#define USB_HID_GAMEPAD_REPORT_ID 1 /* sole report in hid_gamepad_report_desc */
#define USB_HID_PROTOCOL_BOOT 0
#define USB_HID_PROTOCOL_REPORT 1

/*===========================================================================*/
/* USB HID Context                                                           */
/*===========================================================================*/

/**
 * @brief USB HID transport context
 */
static struct
{
    const struct device *hid_dev;       /* HID device from device tree */
    const struct device *fido_dev;      /* Dedicated FIDO HID device (no Report ID) */
    bool initialized;                   /* Transport initialized */
    bool interface_ready;               /* hid_dev_0 (keyboard/mouse/raw) ready */
    bool fido_interface_ready;          /* hid_dev_1 (FIDO) ready */
    bool enabled;                       /* Transport enabled */
    uint8_t protocol;                   /* Current protocol (boot/report) */
    uint8_t idle_rate;                  /* Current idle rate */
    struct k_mutex mutex;               /* Thread safety */
    struct k_sem report_sem;            /* Report completion semaphore (hid_dev_0) */
    struct k_sem fido_report_sem;       /* Report completion semaphore (hid_dev_1) */
    usb_hid_raw_handler_t raw_handler;  /* Raw OUT report handler (Report ID 3) */
    usb_hid_fido_handler_t fido_handler; /* FIDO OUT report handler */
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
    if (ready)
    {
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

    if (type != HID_REPORT_TYPE_INPUT)
        return -ENOTSUP;

    if (id == 1)
    {
        if (len < USB_HID_KEYBOARD_REPORT_SIZE)
            return -ENOBUFS;
        memset(buf, 0, USB_HID_KEYBOARD_REPORT_SIZE);
        return USB_HID_KEYBOARD_REPORT_SIZE;
    }
    else if (id == 2)
    {
        if (len < USB_HID_MOUSE_REPORT_SIZE)
            return -ENOBUFS;
        memset(buf, 0, USB_HID_MOUSE_REPORT_SIZE);
        return USB_HID_MOUSE_REPORT_SIZE;
    }
    else if (id == USB_HID_RAW_REPORT_ID)
    {
        if (len < USB_HID_RAW_REPORT_SIZE)
            return -ENOBUFS;
        memset(buf, 0, USB_HID_RAW_REPORT_SIZE);
        return USB_HID_RAW_REPORT_SIZE;
    }

    return -ENOTSUP;
}

/**
 * @brief Get HID report — dedicated FIDO device (no Report ID, single report)
 */
static int usb_hid_fido_get_report(const struct device *dev,
                                   const uint8_t type, const uint8_t id,
                                   const uint16_t len, uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);

    if (type != HID_REPORT_TYPE_INPUT)
        return -ENOTSUP;

    if (len < USB_HID_FIDO_REPORT_SIZE)
        return -ENOBUFS;
    memset(buf, 0, USB_HID_FIDO_REPORT_SIZE);
    return USB_HID_FIDO_REPORT_SIZE;
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

    if (type != HID_REPORT_TYPE_OUTPUT)
    {
        return 0;
    }

    /* Raw vendor report ID 3 */
    if (id == USB_HID_RAW_REPORT_ID)
    {
        usb_hid_raw_handler_t handler;
        k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
        handler = usb_hid_ctx.raw_handler;
        k_mutex_unlock(&usb_hid_ctx.mutex);
        if (handler && len > 0)
        {
            const uint8_t *data = buf;
            uint16_t dlen = len;
            /* Some Windows HID drivers include the Report ID as the first
             * byte of the SET_REPORT data phase even though the HID spec
             * says it should not be present (the ID is in the SETUP wValue).
             * Strip it BEFORE clamping so the handler always receives a full
             * USB_HID_RAW_PAYLOAD_SIZE-byte payload (not one byte short). */
            if (dlen > 1 && data[0] == USB_HID_RAW_REPORT_ID)
            {
                data++;
                dlen--;
            }
            handler(data, (uint8_t)MIN(dlen, (uint16_t)USB_HID_RAW_PAYLOAD_SIZE));
        }
    }

    return 0;
}

/**
 * @brief Set HID report — dedicated FIDO device (no Report ID, single report)
 */
static int usb_hid_fido_set_report(const struct device *dev,
                                   const uint8_t type, const uint8_t id,
                                   const uint16_t len, const uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);

    if (type != HID_REPORT_TYPE_OUTPUT)
    {
        return 0;
    }

    usb_hid_fido_handler_t handler;
    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    handler = usb_hid_ctx.fido_handler;
    k_mutex_unlock(&usb_hid_ctx.mutex);
    LOG_INF("FIDO SET_REPORT len=%u handler=%p", len, (void *)handler);
    if (handler && len > 0)
    {
        handler(buf, (uint8_t)MIN(len, (uint16_t)USB_HID_FIDO_PAYLOAD_SIZE));
    }

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
 * @brief Output report callback (interrupt OUT endpoint)
 *
 * Called by the USB stack when the host sends data via the interrupt OUT
 * endpoint.  This is the path used by Linux and macOS hidapi (hid_write),
 * as opposed to the SET_REPORT control transfer used by Windows.
 *
 * The data arriving on the interrupt OUT endpoint always includes the
 * Report ID as the first byte when the device uses multiple report IDs,
 * so strip it before forwarding to the raw handler — same logic as
 * usb_hid_set_report().
 */
static void usb_hid_output_report(const struct device *dev,
                                  const uint16_t len, const uint8_t *const buf)
{
    ARG_UNUSED(dev);

    if (len == 0 || buf == NULL)
    {
        return;
    }

    uint8_t report_id = buf[0];

    if (report_id == USB_HID_RAW_REPORT_ID)
    {
        usb_hid_raw_handler_t handler;
        k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
        handler = usb_hid_ctx.raw_handler;
        k_mutex_unlock(&usb_hid_ctx.mutex);
        if (handler)
        {
            const uint8_t *data = buf + 1;
            uint16_t dlen = len - 1;
            handler(data, (uint8_t)MIN(dlen, (uint16_t)USB_HID_RAW_PAYLOAD_SIZE));
        }
    }
}

/**
 * @brief Output report callback — dedicated FIDO device (no Report ID)
 */
static void usb_hid_fido_output_report(const struct device *dev,
                                       const uint16_t len, const uint8_t *const buf)
{
    ARG_UNUSED(dev);

    if (len == 0 || buf == NULL)
    {
        return;
    }

    usb_hid_fido_handler_t handler;
    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    handler = usb_hid_ctx.fido_handler;
    k_mutex_unlock(&usb_hid_ctx.mutex);
    LOG_INF("FIDO OUTPUT_REPORT len=%u handler=%p", len, (void *)handler);
    if (handler)
    {
        handler(buf, (uint8_t)MIN(len, (uint16_t)USB_HID_FIDO_PAYLOAD_SIZE));
    }
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

/**
 * @brief Input report done callback — dedicated FIDO device
 */
static void usb_hid_fido_input_report_done(const struct device *dev,
                                           const uint8_t *const report)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(report);

    k_sem_give(&usb_hid_ctx.fido_report_sem);
}

/**
 * @brief Interface ready callback — dedicated FIDO device
 */
static void usb_hid_fido_iface_ready(const struct device *dev, const bool ready)
{
    ARG_UNUSED(dev);

    LOG_INF("FIDO HID interface %s", ready ? "ready" : "not ready");

    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    usb_hid_ctx.fido_interface_ready = ready;
    if (ready)
    {
        usb_hid_ctx.enabled = true;
    }
    k_mutex_unlock(&usb_hid_ctx.mutex);
}

/* HID device operations structure */
static const struct hid_device_ops usb_hid_ops = {
    .iface_ready = usb_hid_iface_ready,
    .get_report = usb_hid_get_report,
    .set_report = usb_hid_set_report,
    .output_report = usb_hid_output_report,
    .set_idle = usb_hid_set_idle,
    .get_idle = usb_hid_get_idle,
    .set_protocol = usb_hid_set_protocol,
    .input_report_done = usb_hid_input_report_done,
};

/* HID device operations structure — dedicated FIDO device */
static const struct hid_device_ops usb_hid_fido_ops = {
    .iface_ready = usb_hid_fido_iface_ready,
    .get_report = usb_hid_fido_get_report,
    .set_report = usb_hid_fido_set_report,
    .output_report = usb_hid_fido_output_report,
    .set_idle = usb_hid_set_idle,
    .get_idle = usb_hid_get_idle,
    .input_report_done = usb_hid_fido_input_report_done,
};

/*===========================================================================*/
/* Transport Implementation                                                  */
/*===========================================================================*/

static char *type_to_str(hid_device_type_t type)
{
    switch (type)
    {
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

    if (usb_hid_ctx.initialized)
    {
        LOG_WRN("USB HID transport already initialized");
        return 0;
    }

    LOG_INF("Initializing USB HID transport (%s)", type_to_str(device_types));

#if defined(CONFIG_AKIRA_HID_GAMEPAD)
    if (!(device_types & HID_DEVICE_GAMEPAD))
    {
        LOG_ERR("Only gamepad device type is currently supported (AKIRA_HID_MODE_GAMEPAD)");
        return -ENOTSUP;
    }
#else
    if (!(device_types & HID_DEVICE_KEYBOARD))
    {
        LOG_ERR("Only keyboard device type is currently supported");
        return -ENOTSUP;
    }
#endif

    /* Initialize synchronization primitives */
    ret = k_mutex_init(&usb_hid_ctx.mutex);
    if (ret)
    {
        LOG_ERR("Failed to initialize mutex: %d", ret);
        return ret;
    }

    ret = k_sem_init(&usb_hid_ctx.report_sem, 1, 1);
    if (ret)
    {
        LOG_ERR("Failed to initialize semaphore: %d", ret);
        return ret;
    }

    ret = k_sem_init(&usb_hid_ctx.fido_report_sem, 1, 1);
    if (ret)
    {
        LOG_ERR("Failed to initialize FIDO semaphore: %d", ret);
        return ret;
    }

    /* USB manager needs to initialized to get context */
    ret = usb_manager_is_initialized();
    if (!ret)
    {
        LOG_ERR("USB manager must be initialized before HID transport");
        return -EAGAIN;
    }

    usbd_ctx = usb_manager_get_context();
    if (usbd_ctx == NULL)
    {
        LOG_ERR("Failed to get USB device context");
        return -ENODEV;
    }

    /* Get HID device from device tree */
    usb_hid_ctx.hid_dev = DEVICE_DT_GET(DT_NODELABEL(hid_dev_0));
    if (!device_is_ready(usb_hid_ctx.hid_dev))
    {
        LOG_ERR("HID device not ready");
        return -ENODEV;
    }

    /* Register HID device with report descriptor and callbacks */
#if defined(CONFIG_AKIRA_HID_GAMEPAD)
    ret = hid_device_register(usb_hid_ctx.hid_dev,
                              hid_gamepad_report_desc,
                              sizeof(hid_gamepad_report_desc),
                              &usb_hid_ops);
#else
    ret = hid_device_register(usb_hid_ctx.hid_dev,
                              hid_report_desc,
                              sizeof(hid_report_desc),
                              &usb_hid_ops);
#endif
    if (ret)
    {
        LOG_ERR("Failed to register HID device: %d", ret);
        return ret;
    }

    /* Dedicated FIDO HID device — separate interface, no Report ID, so its
     * 64-byte CTAPHID packets fit the full-speed interrupt endpoint cap. */
    usb_hid_ctx.fido_dev = DEVICE_DT_GET(DT_NODELABEL(hid_dev_1));
    if (!device_is_ready(usb_hid_ctx.fido_dev))
    {
        LOG_ERR("FIDO HID device not ready");
        return -ENODEV;
    }

    ret = hid_device_register(usb_hid_ctx.fido_dev,
                              fido_report_desc,
                              sizeof(fido_report_desc),
                              &usb_hid_fido_ops);
    if (ret)
    {
        LOG_ERR("Failed to register FIDO HID device: %d", ret);
        return ret;
    }

    /* Register ALL classes  */
    ret = usbd_register_all_classes(usbd_ctx, USBD_SPEED_FS, 1, NULL);
    if (ret)
    {
        LOG_ERR("Failed to register USB classes: %d", ret);
        return ret;
    }

    /* Finalize USB stack (calls usbd_init()) */
    ret = usb_manager_finalize();
    if (ret)
    {
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

    if (!usb_hid_ctx.initialized)
    {
        LOG_ERR("USB HID transport not initialized");
        return -EINVAL;
    }

    if (usb_hid_ctx.enabled)
    {
        LOG_WRN("USB HID transport already enabled");
        return 0;
    }

    LOG_INF("Enabling USB HID transport");

    /* Enable USB device via USB manager */
    ret = usb_manager_enable();
    if (ret && ret != -EALREADY)
    {
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
    if (!usb_hid_ctx.initialized)
    {
        return 0;
    }

    LOG_INF("Disabling USB HID transport");

    /* Don't clear interface_ready/fido_interface_ready here: those track
     * the actual USB wire state, which iface_ready() callbacks update in
     * response to real host enumeration events. A soft disable/enable
     * cycle (e.g. an app switching HID device types) doesn't drop the USB
     * link, so the host never resends SET_CONFIGURATION and the ready
     * callbacks would never refire to restore them — leaving reports
     * permanently blocked despite a live connection. `enabled` alone is
     * sufficient to gate is_connected() while disabled. */
    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    usb_hid_ctx.enabled = false;
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
 * @brief Check if the dedicated FIDO interface is connected
 */
static bool usb_hid_fido_is_connected(void)
{
    return usb_hid_ctx.enabled && usb_hid_ctx.fido_interface_ready;
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
    static uint8_t __aligned(4) report_buf[USB_HID_KEYBOARD_REPORT_SIZE];

    if (!usb_hid_ctx.initialized || !report)
    {
        return -EINVAL;
    }

    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);

    /* Check if we can send reports */
    if (!usb_hid_ctx.enabled)
    {
        k_mutex_unlock(&usb_hid_ctx.mutex);
        return -EAGAIN;
    }

    if (!usb_hid_ctx.interface_ready)
    {
        k_mutex_unlock(&usb_hid_ctx.mutex);
        return -EAGAIN;
    }

    /* Build HID keyboard report */
    report_buf[0] = 0x01;
    report_buf[1] = report->modifiers;
    report_buf[2] = 0;
    memcpy(&report_buf[3], report->keys, HID_MAX_KEYS); /* Key array (6 bytes) */

    k_mutex_unlock(&usb_hid_ctx.mutex);

    /* Wait for previous report to complete (non-blocking with timeout) */
    ret = k_sem_take(&usb_hid_ctx.report_sem, K_MSEC(100));
    if (ret)
    {
        LOG_WRN("Timeout waiting for previous report to complete");
        return -EBUSY;
    }

    /* Send report via USB HID */
    ret = hid_device_submit_report(usb_hid_ctx.hid_dev,
                                   USB_HID_KEYBOARD_REPORT_SIZE,
                                   report_buf);

    if (ret)
    {
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
 * @brief Send mouse report via USB HID
 *
 * @param report Pointer to mouse report structure
 * @return 0 on success, negative error code on failure
 */
static int usb_hid_transport_send_mouse(const hid_mouse_report_t *report)
{
    int ret;
    static uint8_t __aligned(4) report_buf[USB_HID_MOUSE_REPORT_SIZE];

    if (!usb_hid_ctx.initialized || !report)
    {
        return -EINVAL;
    }

    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);

    /* Check if we can send reports */
    if (!usb_hid_ctx.enabled)
    {
        k_mutex_unlock(&usb_hid_ctx.mutex);
        return -EAGAIN;
    }

    if (!usb_hid_ctx.interface_ready)
    {
        k_mutex_unlock(&usb_hid_ctx.mutex);
        return -EAGAIN;
    }

    /* Build HID mouse report */
    report_buf[0] = 0x02;
    report_buf[1] = report->buttons; /* Button byte */
    report_buf[2] = report->dx;      /* X movement */
    report_buf[3] = report->dy;      /* Y movement */
    report_buf[4] = report->wheel;   /* Wheel movement */

    k_mutex_unlock(&usb_hid_ctx.mutex);

    /* Wait for previous report to complete (non-blocking with timeout) */
    ret = k_sem_take(&usb_hid_ctx.report_sem, K_MSEC(100));
    if (ret)
    {
        LOG_WRN("Timeout waiting for previous report to complete");
        return -EBUSY;
    }

    /* Send report via USB HID */
    ret = hid_device_submit_report(usb_hid_ctx.hid_dev,
                                   USB_HID_MOUSE_REPORT_SIZE,
                                   report_buf);

    if (ret)
    {
        k_sem_give(&usb_hid_ctx.report_sem);
        LOG_ERR("Failed to send mouse report: %d", ret);
        return ret;
    }

    LOG_INF("Sent mouse report: buttons=0x%02x dx=%d dy=%d wheel=%d",
            report->buttons, report->dx, report->dy, report->wheel);

    /* Semaphore will be released in input_report_done callback */

    return 0;
}

/**
 * @brief Send gamepad report via USB HID
 *
 * @param report Pointer to gamepad report structure
 * @return 0 on success, negative error code on failure
 */
static int usb_hid_transport_send_gamepad(const hid_gamepad_report_t *report)
{
    int ret;
    static uint8_t __aligned(4) report_buf[USB_HID_GAMEPAD_REPORT_SIZE];

    if (!usb_hid_ctx.initialized || !report)
    {
        return -EINVAL;
    }

    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);

    if (!usb_hid_ctx.enabled)
    {
        k_mutex_unlock(&usb_hid_ctx.mutex);
        return -EAGAIN;
    }

    if (!usb_hid_ctx.interface_ready)
    {
        k_mutex_unlock(&usb_hid_ctx.mutex);
        return -EAGAIN;
    }

    /* hid_gamepad_report_t is packed to match descriptor byte layout exactly */
    report_buf[0] = USB_HID_GAMEPAD_REPORT_ID;
    memcpy(&report_buf[1], report, sizeof(*report));

    k_mutex_unlock(&usb_hid_ctx.mutex);

    ret = k_sem_take(&usb_hid_ctx.report_sem, K_MSEC(100));
    if (ret)
    {
        LOG_WRN("Timeout waiting for previous report to complete");
        return -EBUSY;
    }

    ret = hid_device_submit_report(usb_hid_ctx.hid_dev,
                                   USB_HID_GAMEPAD_REPORT_SIZE,
                                   report_buf);

    if (ret)
    {
        k_sem_give(&usb_hid_ctx.report_sem);
        LOG_ERR("Failed to send gamepad report: %d", ret);
        return ret;
    }

    LOG_INF("Sent gamepad report: buttons=0x%04x hat=%u",
            report->buttons, report->hat);

    /* Semaphore will be released in input_report_done callback */

    return 0;
}

/**
 * @brief Send a raw report to the host via the active report ID.
 *
 * Routes Report ID 3 through usb_hid_raw_send and Report ID 4 through
 * usb_hid_fido_send so that hid_manager's hid_send_raw_report() works
 * over USB for both the vendor and FIDO channels.
 */
static int usb_hid_transport_send_raw(uint8_t report_id,
                                      const uint8_t *data, size_t len)
{
    if (!data || len == 0)
    {
        return -EINVAL;
    }

    if (report_id == USB_HID_RAW_REPORT_ID)
    {
        if (len > USB_HID_RAW_PAYLOAD_SIZE)
        {
            return -EINVAL;
        }
        /* Pad to full payload size if needed */
        static uint8_t __aligned(4) raw_pad[USB_HID_RAW_PAYLOAD_SIZE];
        memset(raw_pad, 0, sizeof(raw_pad));
        memcpy(raw_pad, data, len);
        return usb_hid_raw_send(raw_pad);
    }

    if (report_id == USB_HID_FIDO_REPORT_ID)
    {
        if (len > USB_HID_FIDO_PAYLOAD_SIZE)
        {
            return -EINVAL;
        }
        static uint8_t __aligned(4) fido_pad[USB_HID_FIDO_PAYLOAD_SIZE];
        memset(fido_pad, 0, sizeof(fido_pad));
        memcpy(fido_pad, data, len);
        return usb_hid_fido_send(fido_pad);
    }

    return -ENOTSUP;
}

/* Transport operations structure for HID manager */
static const hid_transport_ops_t usb_hid_transport_ops = {
    .name = "usb",
    .init = usb_hid_transport_init_fn,
    .enable = usb_hid_transport_enable,
    .disable = usb_hid_transport_disable,
    .is_connected = usb_hid_transport_is_connected,
    .send_mouse = usb_hid_transport_send_mouse,
    .send_keyboard = usb_hid_transport_send_keyboard,
    .send_gamepad = usb_hid_transport_send_gamepad,
    .send_raw = usb_hid_transport_send_raw,
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
    if (ret)
    {
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
    if (!usb_hid_ctx.initialized)
    {
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

/**
 * @brief Register a handler for incoming raw OUT reports (Report ID 3).
 *
 * The callback is invoked from the USB interrupt context, so it must be
 * ISR-safe (no blocking, no heavy work — use a work queue if needed).
 *
 * @param handler Callback, or NULL to unregister.
 */
void usb_hid_raw_set_handler(usb_hid_raw_handler_t handler)
{
    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    usb_hid_ctx.raw_handler = handler;
    k_mutex_unlock(&usb_hid_ctx.mutex);
}

/**
 * @brief Send a raw IN report (Report ID 3) to the host.
 *
 * @param payload  63-byte payload buffer (must be exactly USB_HID_RAW_PAYLOAD_SIZE).
 * @return 0 on success, negative on error.
 */
int usb_hid_raw_send(const uint8_t *payload)
{
    if (!payload)
    {
        return -EINVAL;
    }
    if (!usb_hid_transport_is_connected())
    {
        return -ENOTCONN;
    }

    static uint8_t __aligned(4) report_buf[USB_HID_RAW_REPORT_SIZE];

    int ret = k_sem_take(&usb_hid_ctx.report_sem, K_MSEC(100));
    if (ret)
    {
        return -EBUSY;
    }

    report_buf[0] = USB_HID_RAW_REPORT_ID;
    memcpy(&report_buf[1], payload, USB_HID_RAW_PAYLOAD_SIZE);

    ret = hid_device_submit_report(usb_hid_ctx.hid_dev,
                                   USB_HID_RAW_REPORT_SIZE,
                                   report_buf);
    if (ret)
    {
        k_sem_give(&usb_hid_ctx.report_sem);
        LOG_ERR("Failed to send raw report: %d", ret);
    }
    return ret;
}

/**
 * @brief Register a handler for incoming FIDO OUT reports.
 *
 * The callback is invoked from the USB interrupt context, so it must be
 * ISR-safe (no blocking, no heavy work — use a work queue if needed).
 *
 * @param handler Callback, or NULL to unregister.
 */
void usb_hid_fido_set_handler(usb_hid_fido_handler_t handler)
{
    k_mutex_lock(&usb_hid_ctx.mutex, K_FOREVER);
    usb_hid_ctx.fido_handler = handler;
    k_mutex_unlock(&usb_hid_ctx.mutex);
}

/**
 * @brief Send a FIDO IN report to the host via the dedicated FIDO interface.
 *
 * @param payload  64-byte payload buffer (USB_HID_FIDO_PAYLOAD_SIZE bytes).
 * @return 0 on success, negative on error.
 */
int usb_hid_fido_send(const uint8_t *payload)
{
    if (!payload)
    {
        return -EINVAL;
    }
    if (!usb_hid_fido_is_connected())
    {
        LOG_WRN("FIDO send: transport not connected");
        return -ENOTCONN;
    }

    static uint8_t __aligned(4) report_buf[USB_HID_FIDO_REPORT_SIZE];

    int ret = k_sem_take(&usb_hid_ctx.fido_report_sem, K_MSEC(100));
    if (ret)
    {
        LOG_WRN("FIDO send: report_sem busy");
        return -EBUSY;
    }

    memcpy(report_buf, payload, USB_HID_FIDO_PAYLOAD_SIZE);

    ret = hid_device_submit_report(usb_hid_ctx.fido_dev,
                                   USB_HID_FIDO_REPORT_SIZE,
                                   report_buf);
    if (ret)
    {
        k_sem_give(&usb_hid_ctx.fido_report_sem);
        LOG_ERR("Failed to send FIDO report: %d", ret);
    }
    else
    {
        LOG_INF("FIDO report submitted OK");
    }
    return ret;
}
