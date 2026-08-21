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
 *   U4.7  RST  -> NRST              U4.9 VBAT -> battery +
 *
 * The gauge runs in "mixed mode": the coulomb counter provides accuracy under
 * load while the voltage-mode estimator corrects drift at rest.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(stc3115, CONFIG_FUEL_GAUGE_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Register map                                                        */
/* ------------------------------------------------------------------ */
#define STC3115_REG_MODE          0x00
#define STC3115_REG_CTRL          0x01
#define STC3115_REG_SOC           0x02 /* 16-bit LE, 1/512 %            */
#define STC3115_REG_COUNTER       0x04
#define STC3115_REG_CURRENT       0x06 /* 16-bit LE signed              */
#define STC3115_REG_VOLTAGE       0x08 /* 16-bit LE, 2.20 mV/LSB        */
#define STC3115_REG_TEMPERATURE   0x0A /* 8-bit signed, 1 degC/LSB      */
#define STC3115_REG_AVG_CURRENT   0x0B
#define STC3115_REG_OCV           0x0D /* 16-bit LE, 0.55 mV/LSB        */
#define STC3115_REG_CC_CNF        0x0F /* 16-bit LE                     */
#define STC3115_REG_VM_CNF        0x11 /* 16-bit LE                     */
#define STC3115_REG_ALARM_SOC     0x13
#define STC3115_REG_ALARM_VOLTAGE 0x14
#define STC3115_REG_ID            0x18

#define STC3115_DEVICE_ID         0x14

/* REG_MODE bits */
#define STC3115_MODE_VMODE        BIT(0) /* 1 = voltage-only power saving */
#define STC3115_MODE_ALM_ENA      BIT(3)
#define STC3115_MODE_GG_RUN       BIT(4)

/* REG_CTRL bits */
#define STC3115_CTRL_GG_RST       BIT(1)
#define STC3115_CTRL_GG_VM        BIT(2)
#define STC3115_CTRL_BATFAIL      BIT(3)
#define STC3115_CTRL_PORDET       BIT(4)

/* Fixed-point scaling constants from the STC3115 datasheet.  Both CNF
 * registers are derived from the battery and sense-resistor values:
 *   CC_CNF = (Rsense_mOhm * Capacity_mAh) / 49.556
 *   VM_CNF = (Ri_mOhm     * Capacity_mAh) / 977.78
 * Expressed here as integer numerator/denominator pairs to avoid floats. */
#define STC3115_CC_CNF_NUM        1000
#define STC3115_CC_CNF_DEN        49556
#define STC3115_VM_CNF_NUM        100
#define STC3115_VM_CNF_DEN        97778

/* SOC register is 1/512 of a percent. */
#define STC3115_SOC_PER_PERCENT   512

/* Voltage LSB is 2.20 mV -> microvolts */
#define STC3115_VOLTAGE_UV_NUM    2200

/* Current LSB is 5.88 uV across the sense resistor. Scaled by 100 so that
 * integer maths keeps precision: 5.88 uV -> 588/100. */
#define STC3115_CURRENT_UV_NUM    588
#define STC3115_CURRENT_UV_DEN    100

/* Time for the gauge to produce a first conversion after GG_RUN is set. */
#define STC3115_STARTUP_DELAY_MS  10

struct stc3115_config {
    struct i2c_dt_spec i2c;
    uint16_t sense_mohm;
    uint16_t capacity_mah;
    uint16_t ri_mohm;
};

struct stc3115_data {
    bool present;
};

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

static int stc3115_configure(const struct device *dev)
{
    const struct stc3115_config *cfg = dev->config;
    uint8_t id = 0;
    int ret;

    ret = i2c_reg_read_byte_dt(&cfg->i2c, STC3115_REG_ID, &id);
    if (ret != 0) {
        LOG_DBG("ID read failed: %d", ret);
        return ret;
    }
    if (id != STC3115_DEVICE_ID) {
        LOG_ERR("Unexpected device ID 0x%02x (expected 0x%02x)", id,
                STC3115_DEVICE_ID);
        return -ENODEV;
    }

    /* Derive the battery-specific configuration registers. */
    uint32_t cc_cnf = ((uint32_t)cfg->sense_mohm * cfg->capacity_mah *
                       STC3115_CC_CNF_NUM) / STC3115_CC_CNF_DEN;
    uint32_t vm_cnf = ((uint32_t)cfg->ri_mohm * cfg->capacity_mah *
                       STC3115_VM_CNF_NUM) / STC3115_VM_CNF_DEN;

    cc_cnf = CLAMP(cc_cnf, 1, 0xFFFF);
    vm_cnf = CLAMP(vm_cnf, 1, 0xFFFF);

    uint8_t cc_buf[2] = { (uint8_t)(cc_cnf & 0xFF), (uint8_t)(cc_cnf >> 8) };
    uint8_t vm_buf[2] = { (uint8_t)(vm_cnf & 0xFF), (uint8_t)(vm_cnf >> 8) };

    ret = i2c_burst_write_dt(&cfg->i2c, STC3115_REG_CC_CNF, cc_buf,
                             sizeof(cc_buf));
    if (ret != 0) {
        return ret;
    }
    ret = i2c_burst_write_dt(&cfg->i2c, STC3115_REG_VM_CNF, vm_buf,
                             sizeof(vm_buf));
    if (ret != 0) {
        return ret;
    }

    /* Clear the power-on-reset and battery-fail latches so a fresh
     * measurement cycle starts. */
    ret = i2c_reg_write_byte_dt(&cfg->i2c, STC3115_REG_CTRL,
                                STC3115_CTRL_PORDET);
    if (ret != 0) {
        return ret;
    }

    /* Mixed mode (VMODE clear) with the gauge running and alarms enabled. */
    ret = i2c_reg_write_byte_dt(&cfg->i2c, STC3115_REG_MODE,
                                STC3115_MODE_GG_RUN | STC3115_MODE_ALM_ENA);
    if (ret != 0) {
        return ret;
    }

    k_msleep(STC3115_STARTUP_DELAY_MS);

    LOG_INF("STC3115 @0x%02x ready (Rsense=%u mOhm, %u mAh, "
            "CC_CNF=%u, VM_CNF=%u)",
            cfg->i2c.addr, cfg->sense_mohm, cfg->capacity_mah,
            (unsigned)cc_cnf, (unsigned)vm_cnf);
    return 0;
}

/**
 * @brief Whether a cell is actually attached.
 *
 * The gauge is powered from the 3V3 rail (via D4/R97), so it ACKs on I2C and
 * reports a valid device ID even with J9 empty.  Only a non-zero VOLTAGE
 * reading proves a cell is present; without one the SOC register reads 0 and
 * would otherwise be shown as a genuine 0 % battery.
 */
static bool stc3115_battery_present(const struct device *dev)
{
    uint16_t mv_raw = 0;

    if (stc3115_read16(dev, STC3115_REG_VOLTAGE, &mv_raw) != 0) {
        return false;
    }
    return mv_raw != 0;
}

static int stc3115_get_prop(const struct device *dev, fuel_gauge_prop_t prop,
                            union fuel_gauge_prop_val *val)
{
    const struct stc3115_config *cfg = dev->config;
    struct stc3115_data *data = dev->data;
    uint16_t raw = 0;
    int ret;

    if (!data->present) {
        /* Re-probe periodically so attaching a battery is picked up without
         * a reboot. */
        static int64_t last_probe_ms;
        int64_t now = k_uptime_get();

        if ((now - last_probe_ms) >= 10000) {
            last_probe_ms = now;
            if (stc3115_battery_present(dev) &&
                stc3115_configure(dev) == 0) {
                data->present = true;
                LOG_INF("STC3115: battery attached — gauge running");
            }
        }
        if (!data->present) {
            return -ENODEV;
        }
    } else if (!stc3115_battery_present(dev)) {
        /* Cell removed — stop reporting so the UI hides the gauge rather
         * than showing a misleading 0 %. */
        data->present = false;
        LOG_INF("STC3115: battery removed");
        return -ENODEV;
    }

    switch (prop) {
    case FUEL_GAUGE_VOLTAGE:
        ret = stc3115_read16(dev, STC3115_REG_VOLTAGE, &raw);
        if (ret == 0) {
            /* 2.20 mV/LSB -> microvolts */
            val->voltage = (int)raw * STC3115_VOLTAGE_UV_NUM;
        }
        return ret;

    case FUEL_GAUGE_CURRENT:
    case FUEL_GAUGE_AVG_CURRENT: {
        uint8_t reg = (prop == FUEL_GAUGE_CURRENT) ? STC3115_REG_CURRENT
                                                   : STC3115_REG_AVG_CURRENT;
        ret = stc3115_read16(dev, reg, &raw);
        if (ret == 0) {
            /* LSB = 5.88 uV across Rsense; I(uA) = LSB_uV * 1000 / R_mOhm */
            int32_t uv = ((int32_t)(int16_t)raw * STC3115_CURRENT_UV_NUM) /
                         STC3115_CURRENT_UV_DEN;
            int32_t ua = (uv * 1000) / (int32_t)cfg->sense_mohm;
            if (prop == FUEL_GAUGE_CURRENT) {
                val->current = ua;
            } else {
                val->avg_current = ua;
            }
        }
        return ret;
    }

    case FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE:
        ret = stc3115_read16(dev, STC3115_REG_SOC, &raw);
        if (ret == 0) {
            uint32_t pct = raw / STC3115_SOC_PER_PERCENT;
            val->relative_state_of_charge = (uint8_t)MIN(pct, 100);
        }
        return ret;

    case FUEL_GAUGE_TEMPERATURE: {
        uint8_t t = 0;
        ret = i2c_reg_read_byte_dt(&cfg->i2c, STC3115_REG_TEMPERATURE, &t);
        if (ret == 0) {
            /* 1 degC/LSB signed -> deci-Kelvin */
            val->temperature = (uint16_t)(((int)(int8_t)t + 273) * 10);
        }
        return ret;
    }

    case FUEL_GAUGE_DESIGN_CAPACITY:
        val->design_cap = cfg->capacity_mah;
        return 0;

    default:
        return -ENOTSUP;
    }
}

static DEVICE_API(fuel_gauge, stc3115_api) = {
    .get_property = stc3115_get_prop,
};

static int stc3115_init(const struct device *dev)
{
    const struct stc3115_config *cfg = dev->config;
    struct stc3115_data *data = dev->data;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    /* A missing battery is not a fatal error — get_prop() re-probes and
     * starts reporting once one appears. */
    if (stc3115_configure(dev) != 0) {
        LOG_WRN("STC3115 not responding at init");
    } else if (stc3115_battery_present(dev)) {
        data->present = true;
    } else {
        LOG_INF("STC3115 ready but no cell on J9 — battery gauge hidden");
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
