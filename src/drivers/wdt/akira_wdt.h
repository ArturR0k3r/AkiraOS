/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_WDT_H
#define AKIRA_WDT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the AkiraOS system watchdog.
 *
 * Called automatically via SYS_INIT at APPLICATION level.
 * Installs a WDT timeout channel, enables the watchdog, and starts
 * the auto-feed worker that pets it every CONFIG_AKIRA_WDT_FEED_INTERVAL_MS.
 *
 * @return 0 on success, negative error code on failure.
 */
int akira_wdt_init(void);

/**
 * @brief Manually feed/pet the system watchdog.
 *
 * Safe to call from any context (ISR-safe).  Called internally by the
 * auto-feed worker; also exposed to WASM apps via the "wdt" capability.
 */
void akira_wdt_feed(void);

/**
 * @brief Check whether the system watchdog is active.
 *
 * @return true if WDT was successfully initialized and enabled.
 */
bool akira_wdt_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_WDT_H */
