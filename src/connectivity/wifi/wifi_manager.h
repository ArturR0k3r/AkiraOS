/**
 * @file wifi_manager.h
 * @brief Centralized WiFi connection manager
 *
 * Provides a single authority for WiFi lifecycle: connect, disconnect,
 * credential updates and polling stats. Credentials are stored/loaded
 * via the AkiraOS settings API (NVS).
 *
 * Does NOT replace radio_wifi.c (RAL backend) or wifi_notify.c —
 * it registers its own net_mgmt callbacks independently.
 *
 * @stability stable
 * @since 1.5
 */

#ifndef AKIRA_WIFI_MANAGER_H
#define AKIRA_WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "../../settings/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_STATE_IDLE = 0,
    WIFI_MGR_STATE_CONNECTING,
    WIFI_MGR_STATE_CONNECTED,
    WIFI_MGR_STATE_DISCONNECTING,
} wifi_mgr_state_t;

typedef enum {
    WIFI_MGR_EVT_CONNECTED,      /* IP assigned, fully usable */
    WIFI_MGR_EVT_DISCONNECTED,   /* unexpected drop or clean after disconnect() */
    WIFI_MGR_EVT_CONNECT_FAILED, /* connect request rejected or timed out */
    WIFI_MGR_EVT_SCAN_DONE,      /* scan complete, results ready via wifi_manager_get_scan_results() */
} wifi_mgr_event_t;

typedef void (*wifi_mgr_event_cb_t)(wifi_mgr_event_t event, void *user_data);

typedef struct {
    char    ssid[MAX_VALUE_LEN];
    char    ip_addr[16];         /* dotted decimal, empty if no IP */
    int8_t  rssi;
    uint8_t channel;
    uint32_t time_connected_s;  /* seconds since IP was assigned */
    uint32_t tx_bytes;
    uint32_t rx_bytes;
} wifi_mgr_stats_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t security;   /* enum wifi_security_type value, cast to uint8_t so
                            wifi_mgmt.h does not need to be pulled into every
                            wifi_manager.h consumer */
    uint8_t channel;
} wifi_mgr_scan_result_t;

/**
 * @brief Connect using credentials stored in NVS (system/wifi/creds).
 * @return 0 on success, -ENOENT if no credentials saved, negative errno otherwise.
 */
int wifi_manager_connect(void);

/**
 * @brief Disconnect from the current AP.
 * @return 0 on success, negative errno otherwise.
 */
int wifi_manager_disconnect(void);

/**
 * @brief Save new credentials to NVS via the settings API.
 *
 * PSK is stored encrypted; SSID is stored plain.
 * Does not trigger a reconnect — call wifi_manager_connect() separately.
 *
 * @return 0 on success, negative errno otherwise.
 */
int wifi_manager_update_credentials(const char *ssid, const char *psk);

/**
 * @brief Start an asynchronous network scan.
 *
 * Results accumulate internally as NET_EVENT_WIFI_SCAN_RESULT events arrive.
 * Call wifi_manager_get_scan_results() after receiving a WIFI_MGR_EVT_SCAN_DONE
 * event (via a registered callback) to retrieve them.
 *
 * @return 0 on success, -EBUSY if a scan is already in progress,
 *         -ENODEV if there is no network interface, negative errno otherwise.
 */
int wifi_manager_scan(void);

/**
 * @brief Copy the results of the most recently completed scan.
 *
 * @param out Destination array, must hold at least @p max entries.
 * @param max Maximum number of entries to copy into @p out.
 * @param count_out Set to the number of entries actually copied.
 * @return 0 on success, -EINVAL if out or count_out is NULL.
 */
int wifi_manager_get_scan_results(wifi_mgr_scan_result_t *out, size_t max, size_t *count_out);

/**
 * @brief Poll current connection stats.
 * @return 0 on success, -ENOTCONN if not in CONNECTED state.
 */
int wifi_manager_get_stats(wifi_mgr_stats_t *out);

/**
 * @brief Start a local SoftAP with the given SSID/PSK (open if psk is empty).
 *
 * Used for onboarding: a phone connects to this AP to submit STA
 * credentials via wifi_manager_update_credentials(). Does not affect
 * STA state tracked by wifi_manager_get_state().
 *
 * @return 0 on success, negative errno otherwise.
 */
int wifi_manager_ap_start(const char *ssid, const char *psk);

/**
 * @brief Stop the SoftAP started by wifi_manager_ap_start().
 * @return 0 on success, negative errno otherwise.
 */
int wifi_manager_ap_stop(void);

/**
 * @brief Return the current manager state.
 */
wifi_mgr_state_t wifi_manager_get_state(void);

/**
 * @brief Register an event callback.
 *
 * Up to CONFIG_AKIRA_WIFI_MANAGER_MAX_CBS listeners can be registered.
 * Registering the same (cb, user_data) pair twice is a no-op.
 *
 * @return 0 on success, -EALREADY if already registered, -ENOMEM if table full.
 */
int wifi_manager_register_cb(wifi_mgr_event_cb_t cb, void *user_data);

/**
 * @brief Unregister a previously registered event callback.
 *
 * @return 0 on success, -ENOENT if not found.
 */
int wifi_manager_unregister_cb(wifi_mgr_event_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_WIFI_MANAGER_H */
