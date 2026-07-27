/*
 * Copyright (c) 2026 PenEngineering S.R.L
* SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_SD_CARD_H
#define AKIRA_SD_CARD_H

/**
 * @file sd_card.h
 * @brief SD card hardware init — public API.
 *
 * Hardware-agnostic: the actual SPI-SDHC or native-SDMMC driver is selected
 * via each board's .conf and .overlay.  This module only performs disk probe,
 * FATFS mount, and /SD:/apps directory creation.
 *
 * Enable with CONFIG_AKIRA_SD_CARD=y in the board .conf file.
 * Init runs automatically via SYS_INIT at CONFIG_AKIRA_SD_INIT_PRIORITY (38).
 * @stability stable
 * @since 1.4
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AKIRA_SD_CARD

#ifdef CONFIG_AKIRA_SD_HOTPLUG

/**
 * @brief Callback fired when SD card presence changes.
 * @param present true on insertion (card is mounted by the time this fires),
 *                false on removal (card is already force-unmounted).
 */
typedef void (*akira_sd_hotplug_cb_t)(bool present, void *user_data);

/** @brief Register a callback for insertion/removal events. */
int akira_sd_card_register_hotplug_cb(akira_sd_hotplug_cb_t cb, void *user_data);

/** @brief Unregister a previously registered hotplug callback. */
void akira_sd_card_unregister_hotplug_cb(akira_sd_hotplug_cb_t cb);

/**
 * @brief Register a callback fired on insertion, before the mount attempt.
 *
 * Runs before akira_sd_card_init(), so a UI layer can show a loading
 * indicator immediately when the card is detected.
 */
void akira_sd_card_register_pre_insert_cb(akira_sd_hotplug_cb_t cb, void *user_data);

#endif /* CONFIG_AKIRA_SD_HOTPLUG */

/**
 * @brief Probe disk and mount FATFS at "/SD:".
 *
 * Called automatically via SYS_INIT at APPLICATION level,
 * CONFIG_AKIRA_SD_INIT_PRIORITY (default 38, before fs_manager at 40).
 * Safe to call manually for re-probe after card swap.
 *
 * @return 0 on success, -EIO if disk not found, other negative errno on error.
 */
int akira_sd_card_init(void);

/**
 * @brief Return true if the SD card is currently mounted and accessible.
 */
bool akira_sd_card_is_present(void);

/**
 * @brief Unmount the SD card gracefully.
 *
 * After this call, akira_sd_card_is_present() returns false.
 * Call akira_sd_card_init() to re-mount.
 */
void akira_sd_card_deinit(void);

/**
 * @brief Force-unmount without touching the bus (card physically absent).
 *
 * Zeroes the FATFS object and skips fs_unmount()'s disk I/O, so removal
 * detection never blocks waiting on a card that is already gone.
 */
void akira_sd_card_deinit_force(void);

#else /* !CONFIG_AKIRA_SD_CARD */

/* Stubs for boards where SD is not enabled */
static inline int  akira_sd_card_init(void)       { return -ENOTSUP; }
static inline bool akira_sd_card_is_present(void) { return false; }
static inline void akira_sd_card_deinit(void)      {}
static inline void akira_sd_card_deinit_force(void) {}

#endif /* CONFIG_AKIRA_SD_CARD */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SD_CARD_H */
