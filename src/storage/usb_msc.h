/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef AKIRA_STORAGE_USB_MSC_H_
#define AKIRA_STORAGE_USB_MSC_H_

#include <stdbool.h>

/**
 * @brief True while AkiraOS has released the SD card to the USB MSC shim
 * (i.e. USB_MODE_MSC is active and the shim reports real disk status to the
 * host instead of NOT READY).
 */
bool akira_usb_msc_owns_sd(void);

/**
 * @brief Enter USB drive mode: waits (bounded) for the SD card to be safe
 * to release, unmounts it internally, and switches the USB personality to
 * MSC.
 *
 * @return 0 on success.
 *         -ETIMEDOUT if the SD card was still busy after
 *          CONFIG_AKIRA_USB_MSC_WAIT_TIMEOUT_MS.
 *         -ECANCELED if aborted via akira_usb_msc_cancel_enter().
 *         other negative errno from usb_manager_activate()/fs_unmount().
 */
int akira_usb_msc_enter(void);

/**
 * @brief Abort an in-progress akira_usb_msc_enter() wait. No-op if no
 * enter is currently waiting.
 */
void akira_usb_msc_cancel_enter(void);

/**
 * @brief Exit USB drive mode: switches the USB personality back to IDLE
 * and remounts the SD card internally.
 */
void akira_usb_msc_exit(void);

#endif /* AKIRA_STORAGE_USB_MSC_H_ */
