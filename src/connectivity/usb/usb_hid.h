/**
 * @file usb_hid.h
 * @brief USB HID Device for AkiraOS
 *
 * USB HID device implementation supporting keyboard and gamepad profiles.
 */

#ifndef AKIRA_USB_HID_H
#define AKIRA_USB_HID_H

#include "../hid/hid_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize USB HID device
     * @return 0 on success
     */
    int akira_usb_hid_init(void);

    /**
     * @brief Get USB HID transport operations
     * @return Transport operations pointer
     */
    const hid_transport_ops_t *akira_usb_hid_get_transport(void);

    /**
     * @brief Enable USB HID
     * @return 0 on success
     */
    int usb_hid_enable(void);

    /**
     * @brief Disable USB HID
     * @return 0 on success
     */
    int usb_hid_disable(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_USB_HID_H */
