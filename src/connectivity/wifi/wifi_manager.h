/**
 * @file wifi_manager.h
 * @brief WiFi Manager for AkiraOS
 * 
 * Manages WiFi connectivity and publishes network events.
 */

#ifndef AKIRA_WIFI_MANAGER_H
#define AKIRA_WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi connection status
 */
typedef enum {
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED
} wifi_status_t;

/**
 * @brief WiFi configuration
 */
typedef struct {
    char ssid[33];
    char password[65];
    bool auto_connect;
} wifi_config_t;

/**
 * @brief Initialize WiFi manager
 * 
 * @return 0 on success, negative errno on failure
 */
int wifi_manager_init(void);

/**
 * @brief Connect to WiFi network
 * 
 * @param ssid Network SSID
 * @param password Network password
 * @return 0 on success, negative errno on failure
 */
int wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Disconnect from WiFi
 * 
 * @return 0 on success, negative errno on failure
 */
int wifi_manager_disconnect(void);

/**
 * @brief Get current WiFi status
 * 
 * @return WiFi status
 */
wifi_status_t wifi_manager_get_status(void);

/**
 * @brief Get IP address (if connected)
 * 
 * @param ip_addr Buffer to store IP address string
 * @param len Buffer length
 * @return 0 on success, negative errno on failure
 */
int wifi_manager_get_ip(char *ip_addr, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_WIFI_MANAGER_H */
