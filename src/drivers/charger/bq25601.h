/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bq25601.h
 * @brief BQ25601 driver extras beyond the Zephyr charger API (diagnostics).
 */

#ifndef AKIRA_DRIVERS_CHARGER_BQ25601_H
#define AKIRA_DRIVERS_CHARGER_BQ25601_H

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** REG00..REG0B */
#define BQ25601_NUM_REGS 12

/**
 * @brief Read the whole register file (REG00..REG0B) in one burst.
 * @return 0 on success, negative errno on I2C failure.
 */
int bq25601_read_regs(const struct device *dev, uint8_t regs[BQ25601_NUM_REGS]);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_DRIVERS_CHARGER_BQ25601_H */
