/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file sd_install_screen.h
 * @brief SD card app browser and on-device runner.
 */

#ifndef SD_INSTALL_SCREEN_H
#define SD_INSTALL_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Push the SD app browser onto the shell's screen stack and scan
 *        /SD:/apps/. B (or HOME long-press) pops back.
 */
void sd_install_open(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_INSTALL_SCREEN_H */
