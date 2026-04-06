/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file sd_install_screen.h
 * @brief SD card app browser and on-device installer.
 */

#ifndef SD_INSTALL_SCREEN_H
#define SD_INSTALL_SCREEN_H



#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the SD install screen (call once at boot).
 */
void sd_install_screen_create(void);

/**
 * @brief Navigate to the SD install screen and scan /SD:/apps/.
 */
void sd_install_screen_load(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_LVGL */
#endif /* SD_INSTALL_SCREEN_H */
