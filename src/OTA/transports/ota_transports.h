/**
 * @file ota_transports.h
 * @brief OTA Transport Initializers
 */

#ifndef AKIRA_OTA_TRANSPORTS_H
#define AKIRA_OTA_TRANSPORTS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize HTTP OTA transport
 * @return 0 on success
 */
int ota_http_init(void);

/**
 * @brief Initialize BLE OTA transport
 * @return 0 on success
 */
int ota_ble_init(void);

/**
 * @brief Initialize USB OTA transport
 * @return 0 on success
 */
int ota_usb_init(void);

/**
 * @brief Initialize Cloud OTA transport
 * @return 0 on success
 */
int ota_cloud_init(void);

/**
 * @brief Initialize all available OTA transports
 * @return 0 on success
 */
static inline int ota_transports_init_all(void)
{
    int ret = 0;
    
#if defined(CONFIG_AKIRA_OTA_HTTP) || !defined(CONFIG_AKIRA_OTA_HTTP)
    ret |= ota_http_init();
#endif

#if defined(CONFIG_AKIRA_OTA_BLE) || defined(CONFIG_BT)
    ret |= ota_ble_init();
#endif

#if defined(CONFIG_AKIRA_OTA_USB) || defined(CONFIG_USB_DEVICE_STACK)
    ret |= ota_usb_init();
#endif

#if defined(CONFIG_AKIRA_OTA_CLOUD)
    ret |= ota_cloud_init();
#endif

    return ret;
}

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_OTA_TRANSPORTS_H */
