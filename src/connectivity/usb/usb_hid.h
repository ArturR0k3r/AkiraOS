/**
 * @file usb_hid.h
 * @brief USB HID Transport Header for HID Manager
 * @stability experimental
 * @since 1.4
 */

#ifndef USB_HID_H
#define USB_HID_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*===========================================================================*/
/* Raw HID (Report ID 3) constants                                          */
/*===========================================================================*/

/** Total bytes submitted to hid_device_submit_report (ID + payload) */
#define USB_HID_RAW_REPORT_SIZE 64
/** Usable payload bytes in each raw report */
#define USB_HID_RAW_PAYLOAD_SIZE 63
/** Report ID for vendor raw channel */
#define USB_HID_RAW_REPORT_ID 3

/*===========================================================================*/
/* FIDO/U2F HID constants                                                   */
/*===========================================================================*/

/** Internal channel tag used by hid_manager/usb_hid_transport_send_raw to
 *  route to the FIDO channel. Not a wire Report ID — the FIDO interface has
 *  no Report ID (see hid_dev_1 in the board overlay); CTAPHID requires
 *  exactly 64-byte packets, which a Report ID byte would overflow on
 *  full-speed USB. */
#define USB_HID_FIDO_REPORT_ID      4
/** Payload bytes in each FIDO report (FIDO HID spec: 64 bytes) */
#define USB_HID_FIDO_PAYLOAD_SIZE   64
/** Total bytes submitted to hid_device_submit_report (no Report ID byte) */
#define USB_HID_FIDO_REPORT_SIZE    64

    /*===========================================================================*/
    /* Handler types                                                            */
    /*===========================================================================*/

    /**
     * @brief Callback invoked when a raw OUT report (ID 3) arrives from the host.
     *
     * Called from USB interrupt context — must be ISR-safe (no blocking).
     *
     * @param data   Pointer to the 63-byte payload (does NOT include Report ID byte).
     * @param len    Number of valid bytes in @p data (max USB_HID_RAW_PAYLOAD_SIZE).
     */
    typedef void (*usb_hid_raw_handler_t)(const uint8_t *data, uint8_t len);

    /**
     * @brief Callback invoked when a FIDO OUT report (ID 4) arrives from the host.
     *
     * Called from USB interrupt context — must be ISR-safe (no blocking).
     *
     * @param data   Pointer to the 64-byte payload (does NOT include Report ID byte).
     * @param len    Number of valid bytes in @p data (max USB_HID_FIDO_PAYLOAD_SIZE).
     */
    typedef void (*usb_hid_fido_handler_t)(const uint8_t *data, uint8_t len);

    /*===========================================================================*/
    /* Public API                                                                */
    /*===========================================================================*/

    int usb_hid_transport_init(void);
    const struct device *usb_hid_get_device(void);
    bool usb_hid_is_ready(void);
    uint8_t usb_hid_get_protocol(void);

    /**
     * @brief Register (or unregister) a handler for raw OUT reports (Report ID 3).
     * @param handler Callback, or NULL to unregister.
     */
    void usb_hid_raw_set_handler(usb_hid_raw_handler_t handler);

    /**
     * @brief Send a raw IN report (Report ID 3) to the host.
     * @param payload  63-byte payload (USB_HID_RAW_PAYLOAD_SIZE bytes).
     * @return 0 on success, -ENOTCONN if USB not ready, -EBUSY if timeout.
     */
    int usb_hid_raw_send(const uint8_t *payload);

    /**
     * @brief Register (or unregister) a handler for FIDO OUT reports (Report ID 4).
     * @param handler Callback, or NULL to unregister.
     */
    void usb_hid_fido_set_handler(usb_hid_fido_handler_t handler);

    /**
     * @brief Send a FIDO IN report (Report ID 4) to the host.
     * @param payload  64-byte payload (USB_HID_FIDO_PAYLOAD_SIZE bytes).
     * @return 0 on success, -ENOTCONN if USB not ready, -EBUSY if timeout.
     */
    int usb_hid_fido_send(const uint8_t *payload);

#ifdef __cplusplus
}
#endif

#endif /* USB_HID_H */