/**
 * @file ota_usb.c
 * @brief USB OTA Transport for AkiraOS
 *
 * Receives firmware updates via USB (from PC application).
 */

#include "../ota_transport.h"
#include "../ota_manager.h"
#include "../../connectivity/usb/usb_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#if defined(CONFIG_USB_DEVICE_STACK)
#include <zephyr/usb/usb_device.h>
#define USB_AVAILABLE 1
#else
#define USB_AVAILABLE 0
#endif

LOG_MODULE_REGISTER(ota_usb, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct {
    bool initialized;
    bool enabled;
    ota_transport_state_t state;
    size_t bytes_received;
    size_t total_size;
} usb_ota;

/*===========================================================================*/
/* USB CDC Interface (for firmware transfer)                                 */
/*===========================================================================*/

#if USB_AVAILABLE
/* TODO: Implement USB CDC protocol for OTA */
/* Protocol:
 *   - CMD_START <size> - Start OTA, specify size
 *   - CMD_DATA <data>  - Firmware data chunk
 *   - CMD_END <crc>    - Finalize with CRC check
 *   - CMD_ABORT        - Abort transfer
 */
#endif

/*===========================================================================*/
/* Transport Implementation                                                  */
/*===========================================================================*/

static int usb_init(void)
{
    if (usb_ota.initialized) {
        return 0;
    }
    
    LOG_INF("Initializing USB OTA transport");
    
    memset(&usb_ota, 0, sizeof(usb_ota));
    usb_ota.state = OTA_TRANSPORT_IDLE;
    
    usb_ota.initialized = true;
    return 0;
}

static int usb_deinit(void)
{
    usb_ota.initialized = false;
    usb_ota.enabled = false;
    return 0;
}

static int usb_enable(void)
{
    if (!usb_ota.initialized) {
        return -EINVAL;
    }
    
    usb_ota.enabled = true;
    usb_ota.state = OTA_TRANSPORT_READY;
    
    LOG_INF("USB OTA transport enabled");
    return 0;
}

static int usb_disable(void)
{
    usb_ota.enabled = false;
    usb_ota.state = OTA_TRANSPORT_IDLE;
    
    LOG_INF("USB OTA transport disabled");
    return 0;
}

static bool usb_is_available(void)
{
#if USB_AVAILABLE
    return usb_manager_is_connected();
#else
    return false;
#endif
}

static bool usb_is_active(void)
{
    return usb_ota.state == OTA_TRANSPORT_RECEIVING;
}

static int usb_abort(void)
{
    if (usb_ota.state == OTA_TRANSPORT_RECEIVING) {
        ota_abort_update();
        usb_ota.state = OTA_TRANSPORT_READY;
    }
    return 0;
}

static ota_transport_state_t usb_get_state(void)
{
    return usb_ota.state;
}

/*===========================================================================*/
/* Transport Registration                                                    */
/*===========================================================================*/

static const ota_transport_ops_t usb_transport = {
    .name = "usb",
    .source = OTA_SOURCE_USB,
    .init = usb_init,
    .deinit = usb_deinit,
    .enable = usb_enable,
    .disable = usb_disable,
    .is_available = usb_is_available,
    .is_active = usb_is_active,
    .abort = usb_abort,
    .get_state = usb_get_state
};

int ota_usb_init(void)
{
    return ota_transport_register(&usb_transport);
}
