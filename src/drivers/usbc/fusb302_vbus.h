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

#endif /* AKIRA_DRIVERS_USBC_FUSB302_VBUS_H_ */
