/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_boot_guard.h
 * @brief Automatic rollback of a firmware update that fails to boot.
 *
 * After an OTA update MCUboot swaps the new image into the primary slot and
 * boots it once as a *trial*. If that image is not confirmed before the next
 * reset, MCUboot swaps the previous firmware back. The boot guard decides when
 * to confirm and makes every failure mode end in a reset:
 *
 *  - healthy:  the image reaches AKIRA_HOOK_BOOT_READY (end of akira_start())
 *              and stays up CONFIG_AKIRA_BOOT_GUARD_CONFIRM_DELAY_S seconds,
 *              then it is confirmed and becomes permanent;
 *  - crash:    a fatal error during the trial reboots (see
 *              CONFIG_AKIRA_BOOT_GUARD_FATAL_REBOOT or CONFIG_AKIRA_PANIC);
 *  - hang:     the image is not confirmed within
 *              CONFIG_AKIRA_BOOT_GUARD_TRIAL_TIMEOUT_S, so the guard reboots;
 *  - unhealthy by product rules: call akira_boot_guard_reject().
 *
 * In each failure case MCUboot restores the previous firmware on the following
 * boot, which then reports the rollback through akira_boot_guard_rolled_back()
 * and AKIRA_HOOK_OTA_ERROR (ota.error = -ECANCELED).
 *
 * Requires MCUboot in a swap mode. In overwrite-only mode the previous
 * firmware is already gone once the update is installed, so nothing can be
 * rolled back; the guard still confirms healthy images.
 *
 * Gate: CONFIG_AKIRA_BOOT_GUARD=y
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_BOOT_GUARD_H
#define AKIRA_BOOT_GUARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Evaluate the running image and arm the trial-boot deadline.
 *
 * Runs automatically from SYS_INIT (CONFIG_AKIRA_BOOT_GUARD_INIT_PRIORITY),
 * after settings are available. Exposed for tests.
 *
 * @return 0 on success, negative errno on failure.
 */
int akira_boot_guard_init(void);

/**
 * @brief Confirm the running image so MCUboot keeps it.
 *
 * Called automatically after a healthy boot unless
 * CONFIG_AKIRA_BOOT_GUARD_MANUAL_CONFIRM=y. Does nothing if the image is
 * already confirmed.
 *
 * @return 0 on success, negative errno if the confirmation could not be
 *         written (the trial continues and the deadline still applies).
 */
int akira_boot_guard_confirm(void);

/**
 * @brief Reject the trial image: reboot now so MCUboot restores the previous
 *        firmware.
 *
 * Does not return during a trial boot.
 *
 * @return -EALREADY if the running image is already confirmed.
 */
int akira_boot_guard_reject(void);

/** @brief True while the running image is an unconfirmed trial. */
bool akira_boot_guard_is_trial(void);

/**
 * @brief True if the previous boot's trial image failed and MCUboot rolled
 *        back to the firmware that is running now.
 */
bool akira_boot_guard_rolled_back(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BOOT_GUARD_H */
