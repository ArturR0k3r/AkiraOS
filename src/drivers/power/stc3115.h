/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file stc3115.h
 * @brief STC3115 driver extras beyond the Zephyr fuel_gauge API.
 *
 * Only diagnostics live here — everything a consumer needs at runtime goes
 * through fuel_gauge_get_prop()/fuel_gauge_set_prop().
 */

#ifndef AKIRA_DRIVERS_POWER_STC3115_H
#define AKIRA_DRIVERS_POWER_STC3115_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Raw + decoded register snapshot, for the `power battery` shell command. */
struct stc3115_regs {
    uint8_t  mode;        /**< REG_MODE (GG_RUN=bit4, ALM_ENA=bit3, VMODE=bit0) */
    uint8_t  ctrl;        /**< REG_CTRL (PORDET=bit4, BATFAIL=bit3, GG_VM=bit2, IO0=bit0) */
    uint8_t  id;          /**< REG_ID, 0x14 on a genuine part */
    uint16_t soc_raw;     /**< REG_SOC, 1/512 % */
    uint16_t counter;     /**< REG_COUNTER, conversions since GG_RST (0.5 s each) */
    uint16_t cc_cnf;      /**< REG_CC_CNF as programmed in the chip */
    uint16_t vm_cnf;      /**< REG_VM_CNF as programmed in the chip */
    uint16_t cc_cnf_want; /**< CC_CNF the driver derives from devicetree */
    uint16_t vm_cnf_want; /**< VM_CNF the driver derives from devicetree */
    int32_t  voltage_mv;
    int32_t  current_ma;  /**< + = charging, - = discharging */
    int32_t  ocv_mv;
    int8_t   temperature_c;
    bool     ram_valid;   /**< backup SOC in the chip's RAM passes test word + CRC */
    uint16_t ram_soc_raw; /**< backup SOC from the chip's RAM, 1/512 % */
    bool     present;     /**< driver believes a cell is attached */
    uint32_t restarts;    /**< gauge (re)starts performed since boot */
};

/**
 * @brief Read every register the driver cares about in one locked pass.
 * @return 0 on success, negative errno on I2C failure.
 */
int stc3115_read_regs(const struct device *dev, struct stc3115_regs *out);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_DRIVERS_POWER_STC3115_H */
