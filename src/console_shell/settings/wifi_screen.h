/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file wifi_screen.h
 * @brief WiFi network list and credential entry.
 */

#ifndef SETTINGS_WIFI_SCREEN_H
#define SETTINGS_WIFI_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/** Settings → Wi-Fi: push the network list (starts a scan). */
void wifi_open(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_WIFI_SCREEN_H */
