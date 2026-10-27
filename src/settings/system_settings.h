/**
 * @file system_settings.h
 * @brief Centralized definitions for all system settings keys.
 * 
 */

#ifndef AKIRA_SYSTEM_SETTINGS_H
#define AKIRA_SYSTEM_SETTINGS_H

/* ------------------------------------------------------------------ */
/* System                                                               */
/* ------------------------------------------------------------------ */
/* "ssid\tpsk" — one NVS write guarantees atomicity */
#define AKIRA_SETTINGS_WIFI_CREDS_KEY   "system/wifi/creds"
#define AKIRA_SETTINGS_TIME_BASE_KEY    "system/time_base"

#endif /* AKIRA_SYSTEM_SETTINGS_H */
