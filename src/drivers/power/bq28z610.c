/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_bq28z610

/**
 * @file bq28z610.c
 * @brief TI BQ28Z610DRZ 1-cell Li-Ion fuel gauge + protection driver.
 *
 * The BQ28Z610 implements the SBS (Smart Battery System) 1.1 command set,
 * so all standard property reads use the standard SBS register addresses.
 *
 * I2C address: 0x55 (7-bit).
 * Sense resistor: 1 mΩ (R16 on AkiraConsole Production board).
 *
 * Read protocol: write 1-byte register address, read 2-byte little-endian value.
 * The device uses standard I2C (not SMBus PEC) when accessed over I2C.
 *
 * Supported properties (read-only):
 *   FUEL_GAUGE_VOLTAGE                   → battery voltage (µV)
 *   FUEL_GAUGE_CURRENT                   → instantaneous current (µA, signed)
 *   FUEL_GAUGE_AVG_CURRENT               → 1-minute average current (µA, signed)
 *   FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE  → SoC percentage (0–100)
 *   FUEL_GAUGE_ABSOLUTE_STATE_OF_CHARGE  → absolute SoC (0–100)
 *   FUEL_GAUGE_REMAINING_CAPACITY        → remaining capacity (µAh)
 *   FUEL_GAUGE_FULL_CHARGE_CAPACITY      → full charge capacity (µAh)
 *   FUEL_GAUGE_RUNTIME_TO_EMPTY          → time to empty (minutes)
 *   FUEL_GAUGE_RUNTIME_TO_FULL           → average time to full (minutes)
 *   FUEL_GAUGE_TEMPERATURE               → cell temperature (0.1 K units)
 *   FUEL_GAUGE_CYCLE_COUNT               → charge cycle count
 *   FUEL_GAUGE_PRESENT_STATE             → always FUEL_GAUGE_BATTERY_PRESENT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(bq28z610, CONFIG_FUEL_GAUGE_LOG_LEVEL);

/* =========================================================================
 * SBS register map (all 16-bit, little-endian)
 * ========================================================================= */
#define BQ28Z610_REG_TEMPERATURE            0x06  /* 0.1 K */
#define BQ28Z610_REG_VOLTAGE                0x08  /* mV */
#define BQ28Z610_REG_CURRENT                0x0A  /* mA, signed */
#define BQ28Z610_REG_RELATIVE_SOC           0x0C  /* % */
#define BQ28Z610_REG_ABSOLUTE_SOC           0x0E  /* % */
#define BQ28Z610_REG_REMAINING_CAPACITY     0x10  /* mAh */
#define BQ28Z610_REG_FULL_CHARGE_CAPACITY   0x12  /* mAh */
#define BQ28Z610_REG_RUNTIME_TO_EMPTY       0x14  /* min */
#define BQ28Z610_REG_AVG_TIME_TO_EMPTY      0x16  /* min */
#define BQ28Z610_REG_AVG_TIME_TO_FULL       0x18  /* min */
#define BQ28Z610_REG_AVG_CURRENT            0x1A  /* mA, signed */
#define BQ28Z610_REG_CYCLE_COUNT            0x2A  /* count */
#define BQ28Z610_REG_DESIGN_CAPACITY        0x3C  /* mAh */
#define BQ28Z610_REG_DESIGN_VOLTAGE         0x3E  /* mV */

/* =========================================================================
 * Driver config / data
 * ========================================================================= */
struct bq28z610_config {
    struct i2c_dt_spec i2c;
};

struct bq28z610_data {
    bool     present;   /* false = NACK on init / no battery */
    uint32_t err_count; /* consecutive read errors (for log throttle) */
};

/* ---------- helpers ---------- */

static int bq28z610_reg_read(const struct device *dev, uint8_t reg, uint16_t *val)
{
    const struct bq28z610_config *cfg = dev->config;
    struct bq28z610_data *data = dev->data;
    uint8_t buf[2];
    int ret;

    /* Single attempt — the wakeup sequence in init handles SLEEP mode startup.
     * Retrying here with delays would block the shell thread (called every 1s). */
    ret = i2c_burst_read_dt(&cfg->i2c, reg, buf, sizeof(buf));
    if (ret == 0) {
        data->err_count = 0;
        *val = sys_get_le16(buf);
        return 0;
    }

    data->err_count++;
    /* Log at ERR for first failure, then DBG to avoid flooding the console. */
    if (data->err_count == 1 || (data->err_count % 60) == 0) {
        LOG_WRN("I2C read reg 0x%02X failed: %d (failure #%u)",
                reg, ret, data->err_count);
    } else {
        LOG_DBG("I2C read reg 0x%02X failed: %d", reg, ret);
    }
    return ret;
}

/* ---------- fuel_gauge_get_property ---------- */

static int bq28z610_get_prop(const struct device *dev, fuel_gauge_prop_t prop,
                             union fuel_gauge_prop_val *val)
{
    const struct bq28z610_config *cfg = dev->config;
    struct bq28z610_data *data = dev->data;
    uint16_t raw = 0;
    int ret;

    if (!data->present) {
        /* Re-probe every 10 s so hot-plug / SHUTDOWN recovery is detected fast. */
        static int64_t s_last_probe_ms;
        int64_t now = k_uptime_get();
        if ((now - s_last_probe_ms) >= 10000) {
            s_last_probe_ms = now;
            uint16_t v = 0;
            int probe_ret = i2c_burst_read_dt(&cfg->i2c, BQ28Z610_REG_VOLTAGE,
                                              (uint8_t *)&v, sizeof(v));
            if (probe_ret == 0) {
                data->present   = true;
                data->err_count = 0;
                LOG_INF("BQ28Z610 now responding — Vbat=%u mV",
                        sys_le16_to_cpu(v));
            } else {
                LOG_DBG("BQ28Z610 re-probe failed (err=%d) — "
                        "still no battery", probe_ret);
            }
        }
        if (!data->present) {
            return -ENODEV;
        }
    }

    switch (prop) {
    case FUEL_GAUGE_VOLTAGE:
        ret = bq28z610_reg_read(dev, BQ28Z610_REG_VOLTAGE, &raw);
        if (ret == 0) {
            /* BQ28Z610 reports mV; Zephyr wants µV */
            val->voltage = (int)raw * 1000;
        }
        return ret;

    case FUEL_GAUGE_CURRENT:
        ret = bq28z610_reg_read(dev, BQ28Z610_REG_CURRENT, &raw);
        if (ret == 0) {
            /* BQ28Z610 reports mA (signed); Zephyr wants µA */
            val->current = (int)(int16_t)raw * 1000;
        }
        return ret;

    case FUEL_GAUGE_AVG_CURRENT:
        ret = bq28z610_reg_read(dev, BQ28Z610_REG_AVG_CURRENT, &raw);
        if (ret == 0) {
            val->avg_current = (int)(int16_t)raw * 1000;
        }
        return ret;

    case FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE:
        ret = bq28z610_reg_read(dev, BQ28Z610_REG_RELATIVE_SOC, &raw);
        if (ret == 0) {
            val->relative_state_of_charge = (uint8_t)CLAMP(raw, 0, 100);
        }
        return ret;

    case FUEL_GAUGE_ABSOLUTE_STATE_OF_CHARGE:
        ret = bq28z610_reg_read(dev, BQ28Z610_REG_ABSOLUTE_SOC, &raw);
        if (ret == 0) {
            val->absolute_state_of_charge = (uint8_t)CLAMP(raw, 0, 100);
        }
        return ret;

    case FUEL_GAUGE_REMAINING_CAPACITY:
        ret = bq28z610_reg_read(dev, BQ28Z610_REG_REMAINING_CAPACITY, &raw);
        if (ret == 0) {
            /* BQ28Z610 reports mAh; Zephyr wants µAh */
            val->remaining_capacity = (uint32_t)raw * 1000;
        }
        return ret;

    case FUEL_GAUGE_FULL_CHARGE_CAPACITY:
        ret = bq28z610_reg_read(dev, BQ28Z610_REG_FULL_CHARGE_CAPACITY, &raw);
        if (ret == 0) {
            val->full_charge_capacity = (uint32_t)raw * 1000;
        }
        return ret;

    case FUEL_GAUGE_RUNTIME_TO_EMPTY:
        ret = bq28z610_reg_read(dev, BQ28Z610_REG_RUNTIME_TO_EMPTY, &raw);
        if (ret == 0) {
            /* 0xFFFF = not discharging / time unknown */
            val->runtime_to_empty = (raw == 0xFFFF) ? 0 : (uint32_t)raw;
        }
        return ret;

    case FUEL_GAUGE_RUNTIME_TO_FULL:
        ret = bq28z610_reg_read(dev, BQ28Z610_REG_AVG_TIME_TO_FULL, &raw);
        if (ret == 0) {
            val->runtime_to_full = (raw == 0xFFFF) ? 0 : (uint32_t)raw;
        }
        return ret;

    case FUEL_GAUGE_TEMPERATURE:
        ret = bq28z610_reg_read(dev, BQ28Z610_REG_TEMPERATURE, &raw);
        if (ret == 0) {
            /* BQ28Z610 reports in 0.1 K; Zephyr fuel_gauge uses same unit */
            val->temperature = raw;
        }
        return ret;

    case FUEL_GAUGE_CYCLE_COUNT:
        ret = bq28z610_reg_read(dev, BQ28Z610_REG_CYCLE_COUNT, &raw);
        if (ret == 0) {
            val->cycle_count = (uint32_t)raw;
        }
        return ret;

    case FUEL_GAUGE_PRESENT_STATE:
        /* Device responded on I2C → battery is present */
        val->present_state = true;
        return 0;

    default:
        return -ENOTSUP;
    }
}

/* ---------- init ---------- */

static int bq28z610_init(const struct device *dev)
{
    const struct bq28z610_config *cfg = dev->config;
    struct bq28z610_data *data = dev->data;

    if (!i2c_is_ready_dt(&cfg->i2c)) {
        LOG_ERR("I2C bus %s not ready", cfg->i2c.bus->name);
        return -ENODEV;
    }

    /*
     * BQ28Z610 wakeup: after power-on or SLEEP the device may NACK the first
     * few transactions.  Send a dummy write to ManufacturerAccess (0x00) to
     * nudge it, wait 500ms for internal init, then retry reading voltage.
     * If it still NACKs the gauge is absent or battery is disconnected — mark
     * as not present so callers don't waste I2C bandwidth.
     */
    uint8_t wake[1] = {0x00}; /* ManufacturerAccess register */
    int wake_ret = i2c_write_dt(&cfg->i2c, wake, sizeof(wake));
    /* wake_ret==0: IC acknowledged its address — it has power and is awake.
     * wake_ret==-EFAULT(-14): NACK — IC has no power or is in SHUTDOWN.
     * Either way we continue; the voltage reads below are the real probe. */
    LOG_INF("BQ28Z610 wakeup poke: addr 0x%02X %s (ret=%d)",
            cfg->i2c.addr,
            wake_ret == 0 ? "ACK" : "NACK",
            wake_ret);
    k_sleep(K_MSEC(500));

    uint16_t volt = 0;
    int ret = -1;
    for (int attempt = 0; attempt < 5; attempt++) {
        ret = i2c_burst_read_dt(&cfg->i2c, BQ28Z610_REG_VOLTAGE, (uint8_t *)&volt, 2);
        LOG_DBG("init attempt %d: ret=%d", attempt, ret);
        if (ret == 0) {
            break;
        }
        k_sleep(K_MSEC(200));
    }

    if (ret < 0) {
        LOG_WRN("BQ28Z610 not responding (I2C 0x%02X, err=%d) — "
                "battery absent, gauge unpowered, or protection FETs open; "
                "will retry every 10 s",
                cfg->i2c.addr, ret);
        data->present = false;
        /* Return 0 so the device node stays registered (allows later retry). */
        return 0;
    }

    volt = sys_le16_to_cpu(volt);
    data->present = true;
    LOG_INF("BQ28Z610 ready — Vbat=%u mV", volt);
    return 0;
}

/* ---------- driver registration ---------- */

static DEVICE_API(fuel_gauge, bq28z610_driver_api) = {
    .get_property = bq28z610_get_prop,
};

#define BQ28Z610_INIT(inst)                                                  \
    static const struct bq28z610_config bq28z610_config_##inst = {          \
        .i2c = I2C_DT_SPEC_INST_GET(inst),                                  \
    };                                                                       \
    static struct bq28z610_data bq28z610_data_##inst;                       \
    DEVICE_DT_INST_DEFINE(inst, bq28z610_init, NULL,                        \
                          &bq28z610_data_##inst, &bq28z610_config_##inst,   \
                          POST_KERNEL, CONFIG_FUEL_GAUGE_INIT_PRIORITY,     \
                          &bq28z610_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BQ28Z610_INIT)

/* ---------- APPLICATION-level visibility probe ----------
 * bq28z610_init() runs at POST_KERNEL where the log backend is not yet up,
 * so none of its messages appear in the terminal.  This SYS_INIT runs at
 * APPLICATION level (log always visible) and re-checks the I2C address so
 * the user can see a clear ACK/NACK result — and recovers if the IC was
 * merely sleeping during POST_KERNEL. */
static int bq28z610_app_probe(void)
{
    const struct device *dev = DEVICE_DT_GET_ANY(ti_bq28z610);
    if (!dev) {
        return 0;
    }
    const struct bq28z610_config *cfg = dev->config;
    struct bq28z610_data       *data  = dev->data;

    if (data->present) {
        /* Already responding — nothing to do. */
        return 0;
    }

    /* Raw address probe: START + ADDR + STOP.  ACK = IC powered and alive. */
    uint8_t w = 0x00;
    int addr_ret = i2c_write_dt(&cfg->i2c, &w, 1);

    if (addr_ret == 0) {
        /* Address ACKed — IC has power.  Try a full voltage read. */
        uint16_t v = 0;
        int read_ret = i2c_burst_read_dt(&cfg->i2c, BQ28Z610_REG_VOLTAGE,
                                         (uint8_t *)&v, sizeof(v));
        if (read_ret == 0) {
            data->present   = true;
            data->err_count = 0;
            LOG_INF("BQ28Z610 @ 0x%02X: recovered at APPLICATION level — "
                    "Vbat=%u mV", cfg->i2c.addr, sys_le16_to_cpu(v));
        } else {
            LOG_WRN("BQ28Z610 @ 0x%02X: addr ACK but voltage read failed "
                    "(err=%d) — gauge booting slowly?",
                    cfg->i2c.addr, read_ret);
        }
    } else {
        LOG_WRN("BQ28Z610 @ 0x%02X: addr NACK (err=%d) — IC has no power. "
                "Connect battery to J8 (pin1=PACK, pin2=BAT, pin3=GND).",
                cfg->i2c.addr, addr_ret);
    }
    return 0;
}
/* Priority 85 — runs before power_manager_init (CONFIG_APPLICATION_INIT_PRIORITY=90). */
SYS_INIT(bq28z610_app_probe, APPLICATION, 85);
