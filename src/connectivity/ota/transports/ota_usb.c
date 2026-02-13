/**
 * @file ota_usb.c
 * @brief USB OTA Transport for AkiraOS
 *
 * Receives firmware updates via USB (from PC application).
 */

#include "../ota_transport.h"
/* Removed: #include "../ota_manager.h" - Use interface only */
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

static struct
{
    bool initialized;
    bool enabled;
    ota_transport_state_t state;
    size_t bytes_received;
    size_t total_size;
} usb_ota;

/*===========================================================================*/
/* USB CDC Protocol Implementation                                           */
/*===========================================================================*/

#if USB_AVAILABLE

/* Protocol commands */
#define USB_OTA_CMD_START  0xA1
#define USB_OTA_CMD_DATA   0xA2
#define USB_OTA_CMD_END    0xA3
#define USB_OTA_CMD_ABORT  0xA4
#define USB_OTA_CMD_STATUS 0xA5

/* Response codes */
#define USB_OTA_RESP_OK     0xB1
#define USB_OTA_RESP_ERROR  0xB2
#define USB_OTA_RESP_BUSY   0xB3

/* Buffer for USB data */
#define USB_OTA_BUFFER_SIZE 512
static uint8_t usb_rx_buffer[USB_OTA_BUFFER_SIZE];

/**
 * @brief Handle USB OTA START command
 * Format: [CMD_START][size:4 bytes]
 */
static int usb_handle_start_cmd(const uint8_t *data, size_t len)
{
    if (len < 5) {
        LOG_ERR("Invalid START command length");
        return -EINVAL;
    }
    
    /* Extract firmware size (little-endian) */
    uint32_t size = data[1] | (data[2] << 8) | 
                   (data[3] << 16) | (data[4] << 24);
    
    LOG_INF("USB OTA START: size=%u bytes", size);
    
    /* Start OTA update */
    int ret = ota_start_update(size, OTA_SOURCE_USB);
    if (ret < 0) {
        LOG_ERR("Failed to start OTA: %d", ret);
        usb_ota.state = OTA_TRANSPORT_ERROR;
        return ret;
    }
    
    usb_ota.state = OTA_TRANSPORT_RECEIVING;
    usb_ota.total_size = size;
    usb_ota.bytes_received = 0;
    
    /* Send OK response */
    uint8_t response = USB_OTA_RESP_OK;
    usb_manager_send(&response, 1);
    
    return 0;
}

/**
 * @brief Handle USB OTA DATA command
 * Format: [CMD_DATA][data...]
 */
static int usb_handle_data_cmd(const uint8_t *data, size_t len)
{
    if (usb_ota.state != OTA_TRANSPORT_RECEIVING) {
        LOG_ERR("Not in receiving state");
        uint8_t response = USB_OTA_RESP_ERROR;
        usb_manager_send(&response, 1);
        return -EINVAL;
    }
    
    /* Skip command byte, write firmware data */
    int ret = ota_write_data(data + 1, len - 1);
    if (ret < 0) {
        LOG_ERR("Failed to write OTA data: %d", ret);
        usb_ota.state = OTA_TRANSPORT_ERROR;
        uint8_t response = USB_OTA_RESP_ERROR;
        usb_manager_send(&response, 1);
        return ret;
    }
    
    usb_ota.bytes_received += (len - 1);
    
    /* Log progress every 50KB */
    if ((usb_ota.bytes_received % 51200) == 0) {
        uint32_t progress = (usb_ota.bytes_received * 100) / usb_ota.total_size;
        LOG_INF("USB OTA progress: %u%%", progress);
    }
    
    /* Send OK response */
    uint8_t response = USB_OTA_RESP_OK;
    usb_manager_send(&response, 1);
    
    return 0;
}

/**
 * @brief Handle USB OTA END command
 * Format: [CMD_END][crc:4 bytes]
 */
static int usb_handle_end_cmd(const uint8_t *data, size_t len)
{
    if (usb_ota.state != OTA_TRANSPORT_RECEIVING) {
        LOG_ERR("Not in receiving state");
        uint8_t response = USB_OTA_RESP_ERROR;
        usb_manager_send(&response, 1);
        return -EINVAL;
    }
    
    /* Extract expected CRC (if provided) */
    uint32_t expected_crc = 0;
    if (len >= 5) {
        expected_crc = data[1] | (data[2] << 8) | 
                      (data[3] << 16) | (data[4] << 24);
        LOG_INF("USB OTA END: CRC=0x%08x", expected_crc);
    }
    
    /* Finalize update */
    int ret = ota_finalize_update();
    if (ret < 0) {
        LOG_ERR("Failed to finalize OTA: %d", ret);
        usb_ota.state = OTA_TRANSPORT_ERROR;
        uint8_t response = USB_OTA_RESP_ERROR;
        usb_manager_send(&response, 1);
        return ret;
    }
    
    usb_ota.state = OTA_TRANSPORT_COMPLETE;
    LOG_INF("USB OTA successful! %u bytes received", usb_ota.bytes_received);
    
    /* Send OK response */
    uint8_t response = USB_OTA_RESP_OK;
    usb_manager_send(&response, 1);
    
    return 0;
}

/**
 * @brief Handle USB OTA ABORT command
 */
static int usb_handle_abort_cmd(void)
{
    LOG_INF("USB OTA ABORT");
    ota_abort_update();
    usb_ota.state = OTA_TRANSPORT_READY;
    usb_ota.bytes_received = 0;
    
    uint8_t response = USB_OTA_RESP_OK;
    usb_manager_send(&response, 1);
    return 0;
}

/**
 * @brief Handle USB OTA STATUS command
 */
static int usb_handle_status_cmd(void)
{
    /* Response format: [RESP_OK][state][progress:4 bytes] */
    uint8_t response[6];
    response[0] = USB_OTA_RESP_OK;
    response[1] = (uint8_t)usb_ota.state;
    
    uint32_t progress = 0;
    if (usb_ota.total_size > 0) {
        progress = (usb_ota.bytes_received * 100) / usb_ota.total_size;
    }
    
    response[2] = progress & 0xFF;
    response[3] = (progress >> 8) & 0xFF;
    response[4] = (progress >> 16) & 0xFF;
    response[5] = (progress >> 24) & 0xFF;
    
    usb_manager_send(response, sizeof(response));
    return 0;
}

/**
 * @brief USB data received callback
 */
static void usb_ota_data_received(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    
    uint8_t cmd = data[0];
    
    switch (cmd) {
        case USB_OTA_CMD_START:
            usb_handle_start_cmd(data, len);
            break;
            
        case USB_OTA_CMD_DATA:
            usb_handle_data_cmd(data, len);
            break;
            
        case USB_OTA_CMD_END:
            usb_handle_end_cmd(data, len);
            break;
            
        case USB_OTA_CMD_ABORT:
            usb_handle_abort_cmd();
            break;
            
        case USB_OTA_CMD_STATUS:
            usb_handle_status_cmd();
            break;
            
        default:
            LOG_WRN("Unknown USB OTA command: 0x%02x", cmd);
            uint8_t response = USB_OTA_RESP_ERROR;
            usb_manager_send(&response, 1);
            break;
    }
}

#endif /* USB_AVAILABLE */

/*===========================================================================*/
/* Transport Implementation                                                  */
/*===========================================================================*/

static int usb_init(void)
{
    if (usb_ota.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing USB OTA transport");

    memset(&usb_ota, 0, sizeof(usb_ota));
    usb_ota.state = OTA_TRANSPORT_IDLE;

#if USB_AVAILABLE
    /* Register USB data callback for OTA protocol */
    usb_manager_register_rx_callback(usb_ota_data_received);
    LOG_INF("USB OTA protocol handler registered");
#else
    LOG_WRN("USB not available - USB OTA disabled");
#endif

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
    if (!usb_ota.initialized)
    {
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
    if (usb_ota.state == OTA_TRANSPORT_RECEIVING)
    {
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
    .get_state = usb_get_state};

int ota_usb_init(void)
{
    return ota_transport_register(&usb_transport);
}
