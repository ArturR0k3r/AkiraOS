/**
 * @file akira_wifi.h
 * @brief Station credentials, scanning and soft-AP control
 *
 * For onboarding and device-management code. Link state changes are
 * delivered as AKIRA_HOOK_NET_UP / AKIRA_HOOK_NET_DOWN events (akira_hooks.h).
 * Requires CONFIG_AKIRA_WIFI_MANAGER.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_WIFI_H
#define AKIRA_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One scan result. */
struct akira_wifi_network {
    char ssid[33];
    int8_t rssi;
    uint8_t security; /**< Zephyr enum wifi_security_type */
    uint8_t channel;
};

/** Current link. */
struct akira_wifi_status {
    bool connected;
    int8_t rssi;
    uint8_t channel;
    char ssid[33];
    char ip_addr[16];
};

/** Connect with the stored credentials. -ENOENT if none are stored. */
int akira_wifi_connect(void);
int akira_wifi_disconnect(void);

/** Store station credentials (psk NULL for an open network). */
int akira_wifi_set_credentials(const char *ssid, const char *psk);

/** Start a scan. Results are ready a few seconds later (akira_wifi_scan_results()). */
int akira_wifi_scan(void);

/** Copy the last scan's results. @return number written, or negative errno. */
int akira_wifi_scan_results(struct akira_wifi_network *out, size_t max);

int akira_wifi_get_status(struct akira_wifi_status *out);

/** Start a soft AP. @p psk NULL makes it open (avoid outside development). */
int akira_wifi_ap_start(const char *ssid, const char *psk);
int akira_wifi_ap_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_WIFI_H */
