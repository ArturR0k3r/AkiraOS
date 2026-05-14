/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_boot_guard.h
 * @brief Software boot counter — detects crash loops and triggers OTA rollback.
 *
 * The boot guard persists a per-boot counter in NVS.  On each boot the
 * counter is incremented.  If akira_boot_guard_confirm() is NOT called before
 * the next reset, the counter keeps growing.  Once it reaches
 * CONFIG_AKIRA_OTA_MAX_BOOT_FAILURES, ota_request_rollback() is called.
 *
 * Gate: CONFIG_AKIRA_BOOT_GUARD=y
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_BOOT_GUARD_H
#define AKIRA_BOOT_GUARD_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the boot guard.
 *
 * Must be called early in boot, after NVS has been mounted.
 * Increments the boot counter.  If the counter reaches the configured maximum,
 * ota_request_rollback() is called and the function returns -EAGAIN to
 * indicate rollback was triggered (device will reboot).
 *
 * @return 0 on success, -EAGAIN if rollback was triggered, negative errno on
 *         NVS error (non-fatal: guard is disabled for this boot).
 */
int akira_boot_guard_init(void);

/**
 * @brief Confirm that the current firmware is healthy.
 *
 * Resets the boot counter and calls ota_confirm_firmware() (MCUboot level).
 * Call this once the application has successfully initialised all critical
 * subsystems.
 *
 * @return 0 on success, negative errno on failure.
 */
int akira_boot_guard_confirm(void);

/**
 * @brief Return the current unconfirmed boot count (0 = confirmed).
 */
uint8_t akira_boot_guard_count(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BOOT_GUARD_H */
