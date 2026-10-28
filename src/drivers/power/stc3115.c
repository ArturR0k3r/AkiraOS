/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define DT_DRV_COMPAT st_stc3115

/**
 * @file stc3115.c
 * @brief STMicroelectronics STC3115 Li-Ion fuel gauge — Zephyr fuel_gauge API.
 *
 * Fitted as U4 on AkiraConsole Production at I2C 0x70 (verified against
 * HWAkiraConsole.kicad_pcb and a live bus scan).  The board's earlier DTS
 * claimed a TI BQ28Z610 at 0x55; no such device exists on this hardware.
 *
 * Wiring:
 *   U4.1  ALM  -> BAT_INT (GPIO3, shared with FUSB302 and BQ25601 interrupts)
 *   U4.2  SDA  -> I2C0 SDA          U4.3 SCL -> I2C0 SCL
 *   U4.6  CS   -> R16 (10 mOhm) sense resistor / J9 pin 3
 *   U4.7  RST  -> NRST              U4.8 BATD -> TS via R97, clamped by D4
 *   U4.9  VCC  -> VBAT              U4.10 VIN -> VBAT via R96/C102
 *
 * The gauge runs in "mixed mode": the coulomb counter provides accuracy under
 * load while the voltage-mode estimator corrects drift at rest.
 *
 * Start-up follows ST's reference driver (STSW-BAT001 / STC3115GenericDriver):
 *   - first power-up, POR or battery swap (PORDET/BATFAIL, or the backup in
 *     the chip's RAM is invalid) -> full start: program the gauge in standby,
 *     run it, then re-write OCV so the SOC is re-estimated with our settings;
 *   - MCU reboot while the gauge kept running -> leave it alone, so the SOC
 *     it has been tracking survives the reboot;
 *   - gauge found in standby with a valid RAM backup -> restore that SOC.
 * A periodic health check repeats that decision, so a gauge that drops to
 * standby is restarted instead of reporting a frozen SOC forever.
 *
 * NEVER write CTRL.PORDET=1 casually: on this part that is a soft reset.  An
 * earlier version did exactly that after programming CC_CNF/VM_CNF, which
 * reset the gauge it had just configured and froze the reported SOC.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>

#include "stc3115.h"

LOG_MODULE_REGISTER(stc3115, CONFIG_FUEL_GAUGE_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Register map (STC3115 datasheet / ST reference driver)              */
/* ------------------------------------------------------------------ */
#define STC3115_REG_MODE          0x00
#define STC3115_REG_CTRL          0x01
#define STC3115_REG_SOC           0x02 /* 16-bit LE, 1/512 %            */
#define STC3115_REG_COUNTER       0x04 /* 16-bit LE, conversions        */
#define STC3115_REG_CURRENT       0x06 /* 14-bit signed, 5.88 uV/LSB    */
#define STC3115_REG_VOLTAGE       0x08 /* 12-bit signed, 2.20 mV/LSB    */
#define STC3115_REG_TEMPERATURE   0x0A /* 8-bit signed, 1 degC/LSB      */
#define STC3115_REG_OCV           0x0D /* 14-bit signed, 0.55 mV/LSB    */
#define STC3115_REG_CC_CNF        0x0F /* 16-bit LE                     */
#define STC3115_REG_VM_CNF        0x11 /* 16-bit LE                     */
#define STC3115_REG_CURRENT_THRES 0x15 /* relaxation current threshold */
#define STC3115_REG_ID            0x18
#define STC3115_REG_RAM           0x20 /* 16 bytes, kept until POR      */

#define STC3115_DEVICE_ID         0x14

/* REG_MODE bits */
#define STC3115_MODE_VMODE        BIT(0) /* 1 = voltage-only power saving */
#define STC3115_MODE_ALM_ENA      BIT(3)
#define STC3115_MODE_GG_RUN       BIT(4)

/* REG_CTRL bits */
#define STC3115_CTRL_IO0DATA      BIT(0) /* W: 1 = ALM pin released       */
#define STC3115_CTRL_GG_RST       BIT(1) /* W: reset conversion counter   */
#define STC3115_CTRL_GG_VM        BIT(2) /* R: 1 = voltage mode active    */
#define STC3115_CTRL_BATFAIL      BIT(3) /* R: cell removed / UVLO; W0 clears */
#define STC3115_CTRL_PORDET       BIT(4) /* R: POR seen; W1 = SOFT RESET  */

/* Fixed-point scaling from the ST reference driver, rounded:
 *   CC_CNF = Rsense_mOhm * Capacity_mAh / 49.556
 *   VM_CNF = Ri_mOhm     * Capacity_mAh / 977.78 */
#define STC3115_CC_CNF(r, c)      (((uint32_t)(c) * (r) * 250U + 6194U) / 12389U)
#define STC3115_VM_CNF(ri, c)     (((uint32_t)(c) * (ri) * 50U + 24444U) / 48889U)

/* SOC register is 1/512 of a percent. */
#define STC3115_SOC_PER_PERCENT   512

/* Voltage LSB 2.20 mV, OCV LSB 0.55 mV, current LSB 5.88 uV across Rsense. */
#define STC3115_VOLTAGE_UV_LSB    2200
#define STC3115_OCV_UV_LSB        550
#define STC3115_CURRENT_NV_LSB    5880

/* OCV at or below 3.3 V (6000 x 0.55 mV) is not a credible start point. */
#define STC3115_OCV_MIN_RAW       6000

/* Gauge RAM backup: test word + HRSOC, CRC-8 (poly 0x07) in the last byte. */
#define STC3115_RAM_SIZE          16
#define STC3115_RAM_TESTWORD      0x53A9

/* Below this the cell is effectively empty for this board; within the
 * following 200 mV the SOC is scaled down so 0 % lands at the cutoff. */
#define STC3115_CUTOFF_MV         3300
#define STC3115_CUTOFF_RANGE_MV   200

/* Health check cadence, and how many stalled checks trigger a restart. */
#define STC3115_CHECK_MS          5000
#define STC3115_STALL_CHECKS      2

/* Re-probe interval while no cell is attached. */
#define STC3115_PROBE_MS          10000

struct stc3115_config {
    struct i2c_dt_spec i2c;
    uint16_t sense_mohm;
    uint16_t capacity_mah;
    uint16_t ri_mohm;
};

struct stc3115_data {
    struct k_mutex lock;
    bool present;
    bool probed;
    bool soft_reset_pending;
    int64_t last_probe_ms;
    int64_t last_check_ms;
    uint16_t last_counter;
    uint16_t ram_soc_saved;
    uint8_t stalls;
    uint32_t restarts;
};

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */

static int stc3115_read8(const struct device *dev, uint8_t reg, uint8_t *val)
{
    const struct stc3115_config *cfg = dev->config;

    return i2c_reg_read_byte_dt(&cfg->i2c, reg, val);
}

static int stc3115_write8(const struct device *dev, uint8_t reg, uint8_t val)
{
    const struct stc3115_config *cfg = dev->config;

    return i2c_reg_write_byte_dt(&cfg->i2c, reg, val);
}

static int stc3115_read16(const struct device *dev, uint8_t reg, uint16_t *val)
{
    const struct stc3115_config *cfg = dev->config;
    uint8_t buf[2];
    int ret = i2c_burst_read_dt(&cfg->i2c, reg, buf, sizeof(buf));

    if (ret == 0) {
        /* STC3115 is little-endian: low byte at the lower address. */
        *val = sys_get_le16(buf);
    }
    return ret;
}

static int stc3115_write16(const struct device *dev, uint8_t reg, uint16_t val)
{
    const struct stc3115_config *cfg = dev->config;
    uint8_t buf[2];

    sys_put_le16(val, buf);
    return i2c_burst_write_dt(&cfg->i2c, reg, buf, sizeof(buf));
}

/* Sign-extend an N-bit two's-complement field. */
static inline int32_t stc3115_sext(uint16_t raw, unsigned int bits)
{
    uint16_t mask = (uint16_t)BIT_MASK(bits);
    uint16_t sign = (uint16_t)BIT(bits - 1);

    raw &= mask;
    return (raw & sign) ? (int32_t)raw - (int32_t)BIT(bits) : (int32_t)raw;
}

static int stc3115_read_voltage_mv(const struct device *dev, int32_t *mv)
{
    uint16_t raw;
    int ret = stc3115_read16(dev, STC3115_REG_VOLTAGE, &raw);

    if (ret == 0) {
        *mv = stc3115_sext(raw, 12) * STC3115_VOLTAGE_UV_LSB / 1000;
    }
    return ret;
}

static int stc3115_read_current_ua(const struct device *dev, int32_t *ua)
{
    const struct stc3115_config *cfg = dev->config;
    uint16_t raw;
    int ret = stc3115_read16(dev, STC3115_REG_CURRENT, &raw);

    if (ret == 0) {
        /* I(uA) = LSB_nV / R_mOhm  (nV / mOhm = uA).  Positive = charging,
         * matching the Zephyr fuel_gauge convention. */
        int64_t nv = (int64_t)stc3115_sext(raw, 14) * STC3115_CURRENT_NV_LSB;
        *ua = (int32_t)(nv / (int64_t)cfg->sense_mohm);
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* RAM backup of the SOC (survives MCU resets, lost on gauge POR)      */
/* ------------------------------------------------------------------ */

static int stc3115_ram_read(const struct device *dev, uint16_t *hrsoc, bool *valid)
{
    const struct stc3115_config *cfg = dev->config;
    uint8_t ram[STC3115_RAM_SIZE];
    int ret = i2c_burst_read_dt(&cfg->i2c, STC3115_REG_RAM, ram, sizeof(ram));

    if (ret != 0) {
        return ret;
    }
    *valid = sys_get_le16(&ram[0]) == STC3115_RAM_TESTWORD &&
             crc8_ccitt(0, ram, sizeof(ram) - 1) == ram[sizeof(ram) - 1];
    *hrsoc = sys_get_le16(&ram[2]);
    return 0;
}

static int stc3115_ram_write(const struct device *dev, uint16_t hrsoc, bool valid)
{
    const struct stc3115_config *cfg = dev->config;
    struct stc3115_data *data = dev->data;
    uint8_t ram[STC3115_RAM_SIZE] = { 0 };

    if (valid) {
        sys_put_le16(STC3115_RAM_TESTWORD, &ram[0]);
        sys_put_le16(hrsoc, &ram[2]);
    }
    ram[sizeof(ram) - 1] = crc8_ccitt(0, ram, sizeof(ram) - 1);

    int ret = i2c_burst_write_dt(&cfg->i2c, STC3115_REG_RAM, ram, sizeof(ram));

    if (ret == 0) {
        data->ram_soc_saved = valid ? hrsoc : 0;
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* Start-up / restore                                                  */
/* ------------------------------------------------------------------ */

static void stc3115_cnf(const struct device *dev, uint16_t *cc, uint16_t *vm)
{
    const struct stc3115_config *cfg = dev->config;

    *cc = (uint16_t)CLAMP(STC3115_CC_CNF(cfg->sense_mohm, cfg->capacity_mah), 1, 0xFFFF);
    *vm = (uint16_t)CLAMP(STC3115_VM_CNF(cfg->ri_mohm, cfg->capacity_mah), 1, 0xFFFF);
}

/**
 * Program the algorithm parameters with the gauge in standby, then run it.
 * Does not touch the SOC — callers decide whether to re-estimate it.
 */
static int stc3115_set_param_and_run(const struct device *dev)
{
    const struct stc3115_config *cfg = dev->config;
    struct stc3115_data *data = dev->data;
    uint16_t cc_cnf, vm_cnf;
    int ret;

    stc3115_cnf(dev, &cc_cnf, &vm_cnf);

    /* GG_RUN=0 before changing algorithm parameters. */
    ret = stc3115_write8(dev, STC3115_REG_MODE, STC3115_MODE_VMODE);
    if (ret != 0) {
        return ret;
    }

    /* Relaxation threshold at C/20: LSB = 8 x 5.88 uV across Rsense. */
    uint32_t relax_ma = cfg->capacity_mah / 20U;
    uint32_t relax = (relax_ma * 1000U * cfg->sense_mohm) / (8U * STC3115_CURRENT_NV_LSB);

    ret = stc3115_write8(dev, STC3115_REG_CURRENT_THRES, (uint8_t)MIN(relax, 0xFFU));
    if (ret == 0) {
        ret = stc3115_write16(dev, STC3115_REG_CC_CNF, cc_cnf);
    }
    if (ret == 0) {
        ret = stc3115_write16(dev, STC3115_REG_VM_CNF, vm_cnf);
    }
    if (ret != 0) {
        return ret;
    }

    /* Clear PORDET and BATFAIL (write 0), release the ALM pin and reset the
     * conversion counter.  0x03 is ST's own value for this step. */
    ret = stc3115_write8(dev, STC3115_REG_CTRL,
                         STC3115_CTRL_IO0DATA | STC3115_CTRL_GG_RST);
    if (ret != 0) {
        return ret;
    }

    /* Run in mixed mode.  Alarms stay off: ALM shares BAT_INT with the
     * charger and the USB-C controller, and nothing consumes the alarm — an
     * asserted ALM would only mask their interrupts. */
    ret = stc3115_write8(dev, STC3115_REG_MODE, STC3115_MODE_GG_RUN);
    if (ret == 0) {
        data->last_counter = 0;
        data->stalls = 0;
        data->restarts++;
    }
    return ret;
}

/** Full start: program, run, and re-estimate the SOC from the OCV. */
static int stc3115_startup(const struct device *dev)
{
    uint16_t ocv = 0;
    int ret = stc3115_read16(dev, STC3115_REG_OCV, &ocv);

    if (ret != 0) {
        return ret;
    }
    ocv &= 0x3FFF;

    if (ocv <= STC3115_OCV_MIN_RAW) {
        /* No credible OCV yet — start from 0 % and let the gauge converge. */
        ret = stc3115_write16(dev, STC3115_REG_SOC, 0);
        if (ret == 0) {
            ret = stc3115_set_param_and_run(dev);
        }
    } else {
        ret = stc3115_set_param_and_run(dev);
        if (ret == 0) {
            /* Re-writing OCV restarts the SOC estimate with our parameters. */
            ret = stc3115_write16(dev, STC3115_REG_OCV, ocv);
        }
    }
    if (ret == 0) {
        (void)stc3115_ram_write(dev, 0, true);
        LOG_INF("STC3115 started (OCV %d mV)",
                (int)(stc3115_sext(ocv, 14) * STC3115_OCV_UV_LSB / 1000));
    }
    return ret;
}

/** Restart from a SOC we trust (RAM backup), without re-estimating. */
static int stc3115_restore(const struct device *dev, uint16_t hrsoc)
{
    int ret = stc3115_set_param_and_run(dev);

    if (ret == 0 && hrsoc != 0) {
        ret = stc3115_write16(dev, STC3115_REG_SOC, hrsoc);
    }
    if (ret == 0) {
        (void)stc3115_ram_write(dev, hrsoc, true);
        LOG_INF("STC3115 restored at %u%%",
                (unsigned)((hrsoc + STC3115_SOC_PER_PERCENT / 2) / STC3115_SOC_PER_PERCENT));
    }
    return ret;
}

/**
 * Pick start, restore or leave-running from the chip's current state.
 * Mirrors ST's GasGauge_Initialization() decision, plus a "warm" path for an
 * MCU reboot while the gauge kept running with our parameters.
 */
static int stc3115_bring_up(const struct device *dev)
{
    uint8_t id = 0, mode = 0, ctrl = 0;
    uint16_t cc = 0, vm = 0, cc_want, vm_want, ram_soc = 0;
    bool ram_valid = false;
    int ret;

    ret = stc3115_read8(dev, STC3115_REG_ID, &id);
    if (ret != 0) {
        LOG_DBG("ID read failed: %d", ret);
        return ret;
    }
    if (id != STC3115_DEVICE_ID) {
        LOG_ERR("Unexpected device ID 0x%02x (expected 0x%02x)", id,
                STC3115_DEVICE_ID);
        return -ENODEV;
    }

    ret = stc3115_read8(dev, STC3115_REG_MODE, &mode);
    if (ret == 0) {
        ret = stc3115_read8(dev, STC3115_REG_CTRL, &ctrl);
    }
    if (ret == 0) {
        ret = stc3115_read16(dev, STC3115_REG_CC_CNF, &cc);
    }
    if (ret == 0) {
        ret = stc3115_read16(dev, STC3115_REG_VM_CNF, &vm);
    }
    if (ret == 0) {
        ret = stc3115_ram_read(dev, &ram_soc, &ram_valid);
    }
    if (ret != 0) {
        return ret;
    }
    stc3115_cnf(dev, &cc_want, &vm_want);

    bool latched = (ctrl & (STC3115_CTRL_PORDET | STC3115_CTRL_BATFAIL)) != 0;
    bool running = (mode & STC3115_MODE_GG_RUN) != 0;

    if (!latched && running && cc == cc_want && vm == vm_want) {
        /* Warm MCU reboot: the gauge has been tracking all along.  Only make
         * sure the alarm pin is released — writing 0 to the already-clear
         * PORDET/BATFAIL bits is harmless; writing 1 to PORDET is not. */
        if (mode & STC3115_MODE_ALM_ENA) {
            ret = stc3115_write8(dev, STC3115_REG_MODE,
                                 mode & (uint8_t)~STC3115_MODE_ALM_ENA);
        }
        if (ret == 0 && !(ctrl & STC3115_CTRL_IO0DATA)) {
            ret = stc3115_write8(dev, STC3115_REG_CTRL, STC3115_CTRL_IO0DATA);
        }
        if (ret == 0 && !ram_valid) {
            uint16_t soc = 0;

            if (stc3115_read16(dev, STC3115_REG_SOC, &soc) == 0) {
                (void)stc3115_ram_write(dev, soc, true);
            }
        }
        LOG_INF("STC3115 already running — keeping its SOC");
        return ret;
    }

    if (ram_valid && !latched) {
        return stc3115_restore(dev, ram_soc);
    }
    return stc3115_startup(dev);
}

/**
 * Whether a cell is actually attached.
 *
 * Only a non-zero VOLTAGE reading proves a cell is present; without one the
 * SOC register reads 0 and would otherwise be shown as a genuine 0 % battery.
 */
static bool stc3115_battery_present(const struct device *dev)
{
    uint16_t raw = 0;

    if (stc3115_read16(dev, STC3115_REG_VOLTAGE, &raw) != 0) {
        return false;
    }
    return (raw & 0x0FFF) != 0;
}

/**
 * Periodic self-check, called with the lock held.  Restarts a gauge that has
 * stopped (standby, POR, battery swap, or a counter that no longer moves) and
 * keeps the RAM backup of the SOC fresh.
 */
static void stc3115_health_check(const struct device *dev)
{
    const struct stc3115_config *cfg = dev->config;
    struct stc3115_data *data = dev->data;
    int64_t now = k_uptime_get();
    uint8_t mode = 0, ctrl = 0;
    uint16_t counter = 0, soc = 0;

    if (data->last_check_ms != 0 && (now - data->last_check_ms) < STC3115_CHECK_MS) {
        return;
    }
    data->last_check_ms = now;

    if (stc3115_read8(dev, STC3115_REG_MODE, &mode) != 0 ||
        stc3115_read8(dev, STC3115_REG_CTRL, &ctrl) != 0 ||
        stc3115_read16(dev, STC3115_REG_COUNTER, &counter) != 0 ||
        stc3115_read16(dev, STC3115_REG_SOC, &soc) != 0) {
        return;
    }

    if (data->soft_reset_pending) {
        /* Second half of a battery-swap recovery: the soft reset issued on
         * the previous check has re-measured the new cell's OCV. */
        data->soft_reset_pending = false;
        LOG_INF("STC3115: restarting after soft reset");
        (void)stc3115_startup(dev);
        return;
    }

    if (ctrl & STC3115_CTRL_BATFAIL) {
        /* Cell removed or VCC under-voltage.  As ST does: drop the RAM backup
         * (it belongs to the old cell) and soft-reset so the gauge measures
         * a fresh OCV, then start it on the next check. */
        LOG_WRN("STC3115: BATFAIL — resetting gauge");
        (void)stc3115_ram_write(dev, 0, false);
        (void)stc3115_write8(dev, STC3115_REG_CTRL, STC3115_CTRL_PORDET);
        data->soft_reset_pending = true;
        return;
    }

    if (!(mode & STC3115_MODE_GG_RUN) || (ctrl & STC3115_CTRL_PORDET)) {
        LOG_WRN("STC3115: gauge stopped (MODE=0x%02x CTRL=0x%02x) — restarting",
                mode, ctrl);
        (void)stc3115_bring_up(dev);
        return;
    }

    if (counter == data->last_counter) {
        if (++data->stalls >= STC3115_STALL_CHECKS) {
            LOG_WRN("STC3115: conversion counter stuck at %u — restarting", counter);
            (void)stc3115_restore(dev, soc);
            return;
        }
    } else {
        data->stalls = 0;
    }
    data->last_counter = counter;

    /* Still pushing real charge current but already claiming 100 %: hold at
     * 99 % until the charger terminates (ST's "lately full" compensation). */
    int32_t ua = 0;

    if (soc > 99 * STC3115_SOC_PER_PERCENT &&
        stc3115_read_current_ua(dev, &ua) == 0 &&
        ua > (int32_t)(cfg->capacity_mah / 20U) * 1000) {
        soc = 99 * STC3115_SOC_PER_PERCENT;
        (void)stc3115_write16(dev, STC3115_REG_SOC, soc);
    }

    /* Keep the backup within a percent of the live value. */
    if (abs((int)soc - (int)data->ram_soc_saved) >= STC3115_SOC_PER_PERCENT / 4) {
        (void)stc3115_ram_write(dev, soc, true);
    }
}

/**
 * Presence tracking + health check.  Returns -ENODEV while no cell is
 * attached so callers hide the gauge rather than show a misleading 0 %.
 */
static int stc3115_service(const struct device *dev)
{
    struct stc3115_data *data = dev->data;

    if (!data->present) {
        /* Re-probe periodically so attaching a battery is picked up without
         * a reboot. */
        int64_t now = k_uptime_get();

        if (!data->probed || (now - data->last_probe_ms) >= STC3115_PROBE_MS) {
            data->probed = true;
            data->last_probe_ms = now;
            if (stc3115_battery_present(dev) && stc3115_bring_up(dev) == 0) {
                data->present = true;
                data->last_check_ms = now;
                LOG_INF("STC3115: battery attached — gauge running");
            }
        }
        return data->present ? 0 : -ENODEV;
    }

    if (!stc3115_battery_present(dev)) {
        data->present = false;
        LOG_INF("STC3115: battery removed");
        return -ENODEV;
    }

    stc3115_health_check(dev);
    return 0;
}

/* ------------------------------------------------------------------ */
/* fuel_gauge API                                                      */
/* ------------------------------------------------------------------ */

static int stc3115_get_soc(const struct device *dev, uint8_t *pct_out)
{
    uint16_t raw = 0;
    int32_t mv = 0, ua = 0;
    int ret = stc3115_read16(dev, STC3115_REG_SOC, &raw);

    if (ret != 0) {
        return ret;
    }
    int32_t pct = MIN(((int32_t)raw + STC3115_SOC_PER_PERCENT / 2) / STC3115_SOC_PER_PERCENT, 100);

    /* Early-empty compensation while discharging (ST reference): report 0 %
     * at the cutoff and scale down across the 200 mV above it, so the last
     * percent doesn't linger while the rail is about to brown out. */
    if (stc3115_read_voltage_mv(dev, &mv) == 0 &&
        stc3115_read_current_ua(dev, &ua) == 0 && ua <= 0) {
        if (mv < STC3115_CUTOFF_MV) {
            pct = 0;
        } else if (mv < STC3115_CUTOFF_MV + STC3115_CUTOFF_RANGE_MV) {
            pct = pct * (mv - STC3115_CUTOFF_MV) / STC3115_CUTOFF_RANGE_MV;
        }
    }
    *pct_out = (uint8_t)CLAMP(pct, 0, 100);
    return 0;
}

static int stc3115_get_prop(const struct device *dev, fuel_gauge_prop_t prop,
                            union fuel_gauge_prop_val *val)
{
    const struct stc3115_config *cfg = dev->config;
    struct stc3115_data *data = dev->data;
    int ret;

    k_mutex_lock(&data->lock, K_FOREVER);

    ret = stc3115_service(dev);
    if (ret != 0) {
        goto out;
    }

    switch (prop) {
    case FUEL_GAUGE_VOLTAGE: {
        int32_t mv = 0;

        ret = stc3115_read_voltage_mv(dev, &mv);
        if (ret == 0) {
            val->voltage = mv * 1000;
        }
        break;
    }

    case FUEL_GAUGE_CURRENT: {
        int32_t ua = 0;

        ret = stc3115_read_current_ua(dev, &ua);
        if (ret == 0) {
            val->current = ua;
        }
        break;
    }

    case FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE:
        ret = stc3115_get_soc(dev, &val->relative_state_of_charge);
        break;

    case FUEL_GAUGE_TEMPERATURE: {
        uint8_t t = 0;

        ret = stc3115_read8(dev, STC3115_REG_TEMPERATURE, &t);
        if (ret == 0) {
            /* 1 degC/LSB signed -> deci-Kelvin */
            val->temperature = (uint16_t)(((int)(int8_t)t + 273) * 10);
        }
        break;
    }

    case FUEL_GAUGE_DESIGN_CAPACITY:
        val->design_cap = cfg->capacity_mah;
        break;

    default:
        /* No average-current register on this part. */
        ret = -ENOTSUP;
        break;
    }

out:
    k_mutex_unlock(&data->lock);
    return ret;
}

static int stc3115_set_prop(const struct device *dev, fuel_gauge_prop_t prop,
                            union fuel_gauge_prop_val val)
{
    struct stc3115_data *data = dev->data;
    int ret;

    if (prop != FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE) {
        return -ENOTSUP;
    }

    k_mutex_lock(&data->lock, K_FOREVER);
    if (!data->present) {
        ret = -ENODEV;
    } else {
        /* Used to snap to 100 % when the charger terminates. */
        uint16_t hrsoc = (uint16_t)(MIN(val.relative_state_of_charge, 100) *
                                    STC3115_SOC_PER_PERCENT);

        ret = stc3115_write16(dev, STC3115_REG_SOC, hrsoc);
        if (ret == 0) {
            (void)stc3115_ram_write(dev, hrsoc, true);
        }
    }
    k_mutex_unlock(&data->lock);
    return ret;
}

int stc3115_read_regs(const struct device *dev, struct stc3115_regs *out)
{
    struct stc3115_data *data = dev->data;
    uint16_t v16 = 0;
    uint8_t t = 0;
    int ret;

    memset(out, 0, sizeof(*out));
    k_mutex_lock(&data->lock, K_FOREVER);

    ret = stc3115_read8(dev, STC3115_REG_MODE, &out->mode);
    if (ret == 0) {
        ret = stc3115_read8(dev, STC3115_REG_CTRL, &out->ctrl);
    }
    if (ret == 0) {
        ret = stc3115_read8(dev, STC3115_REG_ID, &out->id);
    }
    if (ret == 0) {
        ret = stc3115_read16(dev, STC3115_REG_SOC, &out->soc_raw);
    }
    if (ret == 0) {
        ret = stc3115_read16(dev, STC3115_REG_COUNTER, &out->counter);
    }
    if (ret == 0) {
        ret = stc3115_read16(dev, STC3115_REG_CC_CNF, &out->cc_cnf);
    }
    if (ret == 0) {
        ret = stc3115_read16(dev, STC3115_REG_VM_CNF, &out->vm_cnf);
    }
    if (ret == 0) {
        ret = stc3115_read_voltage_mv(dev, &out->voltage_mv);
    }
    if (ret == 0) {
        ret = stc3115_read_current_ua(dev, &out->current_ma);
    }
    if (ret == 0) {
        ret = stc3115_read16(dev, STC3115_REG_OCV, &v16);
    }
    if (ret == 0) {
        ret = stc3115_read8(dev, STC3115_REG_TEMPERATURE, &t);
    }
    if (ret == 0) {
        ret = stc3115_ram_read(dev, &out->ram_soc_raw, &out->ram_valid);
    }

    out->current_ma /= 1000;
    out->ocv_mv = stc3115_sext(v16, 14) * STC3115_OCV_UV_LSB / 1000;
    out->temperature_c = (int8_t)t;
    stc3115_cnf(dev, &out->cc_cnf_want, &out->vm_cnf_want);
    out->present = data->present;
    out->restarts = data->restarts;

    k_mutex_unlock(&data->lock);
    return ret;
}

int stc3115_standby(const struct device *dev)
{
    struct stc3115_data *data = dev->data;
    uint16_t soc = 0;
    int ret;

    k_mutex_lock(&data->lock, K_FOREVER);
    /* Save where we are, then stop converting.  At the next boot
     * stc3115_bring_up() finds GG_RUN=0 with a valid RAM backup and restores
     * this SOC rather than re-estimating it from a fresh OCV. */
    ret = stc3115_read16(dev, STC3115_REG_SOC, &soc);
    if (ret == 0) {
        (void)stc3115_ram_write(dev, soc, true);
        ret = stc3115_write8(dev, STC3115_REG_MODE, STC3115_MODE_VMODE);
    }
    k_mutex_unlock(&data->lock);
    return ret;
}

static DEVICE_API(fuel_gauge, stc3115_api) = {
    .get_property = stc3115_get_prop,
    .set_property = stc3115_set_prop,
};

static int stc3115_init(const struct device *dev)
{
    const struct stc3115_config *cfg = dev->config;
    struct stc3115_data *data = dev->data;

    k_mutex_init(&data->lock);

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    /* A missing battery is not a fatal error — get_prop() re-probes and
     * starts reporting once one appears. */
    if (!stc3115_battery_present(dev)) {
        LOG_INF("STC3115: no cell on J9 — battery gauge hidden");
    } else if (stc3115_bring_up(dev) != 0) {
        LOG_WRN("STC3115 not responding at init");
    } else {
        data->present = true;
        data->probed = true;
        data->last_probe_ms = k_uptime_get();
    }
    return 0;
}

#define STC3115_INIT(inst)                                                     \
    static struct stc3115_data stc3115_data_##inst;                            \
    static const struct stc3115_config stc3115_config_##inst = {               \
        .i2c          = I2C_DT_SPEC_INST_GET(inst),                            \
        .sense_mohm   = DT_INST_PROP(inst, sense_resistor_milliohm),           \
        .capacity_mah = DT_INST_PROP(inst, design_capacity_mah),               \
        .ri_mohm      = DT_INST_PROP(inst, internal_resistance_milliohm),      \
    };                                                                         \
    DEVICE_DT_INST_DEFINE(inst, stc3115_init, NULL, &stc3115_data_##inst,      \
                          &stc3115_config_##inst, POST_KERNEL,                 \
                          CONFIG_FUEL_GAUGE_INIT_PRIORITY, &stc3115_api);

DT_INST_FOREACH_STATUS_OKAY(STC3115_INIT)
