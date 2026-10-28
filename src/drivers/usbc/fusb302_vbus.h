/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef AKIRA_DRIVERS_USBC_FUSB302_VBUS_H_
#define AKIRA_DRIVERS_USBC_FUSB302_VBUS_H_

#include <stdbool.h>

/**
 * @brief True if the FUSB302 currently reports VBUS present (STATUS0.VBUSOK).
 *
 * Returns false if the chip hasn't been reached (e.g. I2C read failed) —
 * a missing/unreadable sense chip must never look like "cable present".
 */
bool akira_fusb302_vbus_present(void);

/**
 * @brief Pause or resume the periodic VBUS poll (screen-blank gating).
 *
 * @param idle true  → stop the periodic poll to save I2C/CPU wake-ups while
 *                     blanked; the GPIO3 (INT_N) falling-edge interrupt still
 *                     services real plug-in / charger edges.
 *             false → resume polling and run one check immediately.
 *
 * Safe to call repeatedly. The deep-sleep path should use
 * akira_fusb302_power_down() instead.
 */
void akira_fusb302_set_idle(bool idle);

/**
 * @brief Drop the FUSB302 to POWER=0x00 and stop polling before deep sleep.
 *
 * The SoC reboots on wake and the driver re-initialises from scratch, so no
 * state is restored. Call this from the deep-sleep entry path only.
 */
void akira_fusb302_power_down(void);

#endif /* AKIRA_DRIVERS_USBC_FUSB302_VBUS_H_ */
