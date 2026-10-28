/**
 * @file bq25601.c
 * @brief TI BQ25601RTW Li-Ion battery charger driver
 *
 * Implements the Zephyr charger API (zephyr/drivers/charger.h).
 * I2C address: 0x6B (7-bit).  ~INT / STAT → PWR_INT → GPIO3.
 * PSEL pulled HIGH on AkiraConsole Production → 500 mA default
 * input current limit (adjustable via CHARGER_PROP_INPUT_REGULATION_CURRENT_UA).
 *
 * Supported properties:
 *   CHARGER_PROP_STATUS                  (read)
 *   CHARGER_PROP_ONLINE                  (read)
 *   CHARGER_PROP_CHARGE_TYPE             (read)
 *   CHARGER_PROP_HEALTH                  (read)
 *   CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA  (read/write, 0–3000 mA)
 *   CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV  (read/write, 3856–4624 mV)
 *   CHARGER_PROP_INPUT_REGULATION_CURRENT_UA (read/write, 100–3200 mA)
 *
 * Register layout per the BQ25601 datasheet (cross-checked against Linux
 * bq256xx_charger.c).  An earlier version used a BQ24190-style map: it read
 * the part number from REG0A, so init failed with -ENODEV and the OS never
 * knew whether it was charging.
 */

#define DT_DRV_COMPAT ti_bq25601

#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "bq25601.h"

LOG_MODULE_REGISTER(bq25601, CONFIG_CHARGER_LOG_LEVEL);

/* =========================================================================
 * Register map
 * ========================================================================= */
#define BQ25601_REG00  0x00  /* Input Current Limit           */
#define BQ25601_REG01  0x01  /* Charger Control 0             */
#define BQ25601_REG02  0x02  /* Charge Current Limit          */
#define BQ25601_REG03  0x03  /* Pre-Charge / Term Current     */
#define BQ25601_REG04  0x04  /* Battery Voltage Limit         */
#define BQ25601_REG05  0x05  /* Charger Control 1             */
#define BQ25601_REG06  0x06  /* Charger Control 2             */
#define BQ25601_REG07  0x07  /* Charger Control 3             */
#define BQ25601_REG08  0x08  /* Charger Status 0              */
#define BQ25601_REG09  0x09  /* Charger Status 1 (faults)     */
#define BQ25601_REG0A  0x0A  /* Charger Status 2              */
#define BQ25601_REG0B  0x0B  /* Part Information              */

/* REG00 bits */
#define BQ25601_REG00_EN_HIZ       BIT(7)
#define BQ25601_REG00_IINDPM_MASK  GENMASK(4, 0)
/* IINDPM: 00000=100mA .. step 100mA .. 11111=3200mA */
#define BQ25601_IINDPM_OFFSET_MA   100
#define BQ25601_IINDPM_STEP_MA     100
#define BQ25601_IINDPM_MAX_MA      3200

/* REG01 bits */
#define BQ25601_REG01_WD_RST     BIT(6)
#define BQ25601_REG01_OTG_CONFIG BIT(5)
#define BQ25601_REG01_CHG_CONFIG BIT(4)
#define BQ25601_REG01_SYS_MIN_MASK  GENMASK(3, 1)

/* REG02 bits */
#define BQ25601_REG02_ICHG_MASK  GENMASK(5, 0)
/* ICHG: 000000=0mA .. step 60mA .. (3000mA max) */
#define BQ25601_ICHG_STEP_MA   60

/* REG04 bits — VRECHG (bit 0) and TOPOFF_TIMER (bits 2:1) are preserved */
#define BQ25601_REG04_VREG_MASK  GENMASK(7, 3)
#define BQ25601_REG04_VREG_SHIFT 3
/* VREG: 00000=3856mV .. step 32mV .. 11000=4624mV (default 01011 = 4208mV) */
#define BQ25601_VREG_OFFSET_MV  3856
#define BQ25601_VREG_STEP_MV    32
#define BQ25601_VREG_MAX_MV     4624

/* REG05 bits */
#define BQ25601_REG05_EN_TERM        BIT(7)
#define BQ25601_REG05_WATCHDOG_MASK  GENMASK(5, 4) /* 00 = disabled, 01 = 40 s */
#define BQ25601_REG05_EN_TIMER       BIT(3)

/* REG08: System Status */
#define BQ25601_REG08_VBUS_STAT_MASK  GENMASK(7, 5)
#define BQ25601_REG08_CHG_STAT_MASK   GENMASK(4, 3)
#define BQ25601_REG08_PG_STAT         BIT(2)
#define BQ25601_REG08_THERM_STAT      BIT(1)
#define BQ25601_REG08_VSYS_STAT       BIT(0)

#define BQ25601_VBUS_NO_INPUT   0x00
#define BQ25601_VBUS_USB_SDP    0x20
#define BQ25601_VBUS_USB_CDP    0x40
#define BQ25601_VBUS_USB_DCP    0x60
#define BQ25601_VBUS_OTG        0xE0

#define BQ25601_CHG_NOT_CHARGING 0x00
#define BQ25601_CHG_PRECHARGE    0x08
#define BQ25601_CHG_FAST_CHARGE  0x10
#define BQ25601_CHG_DONE         0x18

/* REG09: Fault */
#define BQ25601_REG09_WATCHDOG_FAULT  BIT(7)
#define BQ25601_REG09_BOOST_FAULT     BIT(6)
#define BQ25601_REG09_CHG_FAULT_MASK  GENMASK(5, 4)
#define BQ25601_REG09_BAT_FAULT       BIT(3)
#define BQ25601_REG09_NTC_FAULT_MASK  GENMASK(2, 0)

#define BQ25601_CHG_FAULT_THERMAL     0x20
#define BQ25601_CHG_FAULT_TIMER       0x30
#define BQ25601_NTC_WARM              0x02
#define BQ25601_NTC_COOL              0x03
#define BQ25601_NTC_COLD              0x05
#define BQ25601_NTC_HOT               0x06

/* REG0B: Part number 0010 = BQ25601 */
#define BQ25601_REG0B_PN_MASK   GENMASK(6, 3)
#define BQ25601_REG0B_PN_SHIFT  3
#define BQ25601_PART_NUMBER     0x02

/* =========================================================================
 * Driver config / data
 * ========================================================================= */
struct bq25601_config {
    struct i2c_dt_spec  i2c;
    struct gpio_dt_spec int_gpio;
    uint32_t            init_ichg_ua;
    uint32_t            init_vreg_uv;
};

struct bq25601_data {
    bool initialized;
};

/* =========================================================================
 * Helpers
 * ========================================================================= */
static inline int bq25601_read(const struct device *dev, uint8_t reg, uint8_t *val)
{
    const struct bq25601_config *cfg = dev->config;
    return i2c_reg_read_byte_dt(&cfg->i2c, reg, val);
}

static inline int bq25601_write(const struct device *dev, uint8_t reg, uint8_t val)
{
    const struct bq25601_config *cfg = dev->config;
    return i2c_reg_write_byte_dt(&cfg->i2c, reg, val);
}

static inline int bq25601_update(const struct device *dev, uint8_t reg,
                                  uint8_t mask, uint8_t val)
{
    const struct bq25601_config *cfg = dev->config;
    return i2c_reg_update_byte_dt(&cfg->i2c, reg, mask, val);
}

/* External power is present: power-good, or any VBUS source except our own
 * OTG boost output. */
static inline bool bq25601_input_present(uint8_t reg08)
{
    uint8_t vbus = reg08 & BQ25601_REG08_VBUS_STAT_MASK;

    return (reg08 & BQ25601_REG08_PG_STAT) ||
           (vbus != BQ25601_VBUS_NO_INPUT && vbus != BQ25601_VBUS_OTG);
}

int bq25601_read_regs(const struct device *dev, uint8_t regs[BQ25601_NUM_REGS])
{
    const struct bq25601_config *cfg = dev->config;

    return i2c_burst_read_dt(&cfg->i2c, BQ25601_REG00, regs, BQ25601_NUM_REGS);
}

/* =========================================================================
 * Charger API
 * ========================================================================= */
static int bq25601_charge_enable(const struct device *dev, bool enable)
{
    return bq25601_update(dev, BQ25601_REG01, BQ25601_REG01_CHG_CONFIG,
                          enable ? BQ25601_REG01_CHG_CONFIG : 0);
}

static int bq25601_get_prop(const struct device *dev,
                             charger_prop_t prop,
                             union charger_propval *val)
{
    uint8_t reg08 = 0, reg09 = 0;
    int ret;

    ret = bq25601_read(dev, BQ25601_REG08, &reg08);
    if (ret < 0) {
        return ret;
    }

    switch (prop) {
    case CHARGER_PROP_ONLINE:
        val->online = bq25601_input_present(reg08)
                      ? CHARGER_ONLINE_FIXED : CHARGER_ONLINE_OFFLINE;
        return 0;

    case CHARGER_PROP_STATUS: {
        uint8_t chg_stat = reg08 & BQ25601_REG08_CHG_STAT_MASK;
        if (!bq25601_input_present(reg08)) {
            val->status = CHARGER_STATUS_DISCHARGING;
        } else if (chg_stat == BQ25601_CHG_DONE) {
            val->status = CHARGER_STATUS_FULL;
        } else if (chg_stat != BQ25601_CHG_NOT_CHARGING) {
            val->status = CHARGER_STATUS_CHARGING;
        } else {
            val->status = CHARGER_STATUS_NOT_CHARGING;
        }
        return 0;
    }

    case CHARGER_PROP_CHARGE_TYPE: {
        uint8_t chg_stat = reg08 & BQ25601_REG08_CHG_STAT_MASK;
        if (chg_stat == BQ25601_CHG_PRECHARGE) {
            val->charge_type = CHARGER_CHARGE_TYPE_TRICKLE;
        } else if (chg_stat == BQ25601_CHG_FAST_CHARGE) {
            val->charge_type = CHARGER_CHARGE_TYPE_FAST;
        } else {
            val->charge_type = CHARGER_CHARGE_TYPE_NONE;
        }
        return 0;
    }

    case CHARGER_PROP_HEALTH: {
        ret = bq25601_read(dev, BQ25601_REG09, &reg09);
        if (ret < 0) {
            return ret;
        }
        uint8_t ntc = reg09 & BQ25601_REG09_NTC_FAULT_MASK;
        uint8_t chg_fault = reg09 & BQ25601_REG09_CHG_FAULT_MASK;
        if (reg09 & BQ25601_REG09_BAT_FAULT) {
            val->health = CHARGER_HEALTH_OVERVOLTAGE;
        } else if (ntc == BQ25601_NTC_HOT) {
            val->health = CHARGER_HEALTH_OVERHEAT;
        } else if (ntc == BQ25601_NTC_COLD) {
            val->health = CHARGER_HEALTH_COLD;
        } else if (ntc == BQ25601_NTC_WARM) {
            val->health = CHARGER_HEALTH_WARM;
        } else if (ntc == BQ25601_NTC_COOL) {
            val->health = CHARGER_HEALTH_COOL;
        } else if (chg_fault == BQ25601_CHG_FAULT_TIMER) {
            val->health = CHARGER_HEALTH_SAFETY_TIMER_EXPIRE;
        } else if (chg_fault == BQ25601_CHG_FAULT_THERMAL ||
                   (reg08 & BQ25601_REG08_THERM_STAT)) {
            val->health = CHARGER_HEALTH_WARM;
        } else {
            val->health = CHARGER_HEALTH_GOOD;
        }
        return 0;
    }

    case CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA: {
        uint8_t reg02 = 0;
        ret = bq25601_read(dev, BQ25601_REG02, &reg02);
        if (ret < 0) {
            return ret;
        }
        val->const_charge_current_ua =
            (reg02 & BQ25601_REG02_ICHG_MASK) * BQ25601_ICHG_STEP_MA * 1000;
        return 0;
    }

    case CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV: {
        uint8_t reg04 = 0;
        ret = bq25601_read(dev, BQ25601_REG04, &reg04);
        if (ret < 0) {
            return ret;
        }
        uint8_t vreg = (reg04 & BQ25601_REG04_VREG_MASK) >> BQ25601_REG04_VREG_SHIFT;
        val->const_charge_voltage_uv =
            (BQ25601_VREG_OFFSET_MV + vreg * BQ25601_VREG_STEP_MV) * 1000;
        return 0;
    }

    case CHARGER_PROP_INPUT_REGULATION_CURRENT_UA: {
        uint8_t reg00 = 0;
        ret = bq25601_read(dev, BQ25601_REG00, &reg00);
        if (ret < 0) {
            return ret;
        }
        uint8_t iindpm = reg00 & BQ25601_REG00_IINDPM_MASK;
        val->input_current_regulation_current_ua =
            (BQ25601_IINDPM_OFFSET_MA + iindpm * BQ25601_IINDPM_STEP_MA) * 1000;
        return 0;
    }

    default:
        return -ENOTSUP;
    }
}

static int bq25601_set_prop(const struct device *dev,
                             charger_prop_t prop,
                             const union charger_propval *val)
{
    switch (prop) {
    case CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA: {
        uint32_t ma = val->const_charge_current_ua / 1000;
        ma = CLAMP(ma, 0U, 3000U);
        uint8_t ichg = (uint8_t)(ma / BQ25601_ICHG_STEP_MA);
        return bq25601_update(dev, BQ25601_REG02, BQ25601_REG02_ICHG_MASK, ichg);
    }

    case CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV: {
        uint32_t mv = val->const_charge_voltage_uv / 1000;
        mv = CLAMP(mv, (uint32_t)BQ25601_VREG_OFFSET_MV, (uint32_t)BQ25601_VREG_MAX_MV);
        uint8_t vreg = (uint8_t)((mv - BQ25601_VREG_OFFSET_MV) / BQ25601_VREG_STEP_MV);
        return bq25601_update(dev, BQ25601_REG04, BQ25601_REG04_VREG_MASK,
                              (uint8_t)(vreg << BQ25601_REG04_VREG_SHIFT));
    }

    case CHARGER_PROP_INPUT_REGULATION_CURRENT_UA: {
        uint32_t ma = val->input_current_regulation_current_ua / 1000;
        ma = CLAMP(ma, (uint32_t)BQ25601_IINDPM_OFFSET_MA, (uint32_t)BQ25601_IINDPM_MAX_MA);
        uint8_t iindpm = (uint8_t)((ma - BQ25601_IINDPM_OFFSET_MA) / BQ25601_IINDPM_STEP_MA);
        return bq25601_update(dev, BQ25601_REG00, BQ25601_REG00_IINDPM_MASK, iindpm);
    }

    default:
        return -ENOTSUP;
    }
}


/* =========================================================================
 * Init
 * ========================================================================= */
static int bq25601_init(const struct device *dev)
{
    const struct bq25601_config *cfg = dev->config;

    if (!i2c_is_ready_dt(&cfg->i2c)) {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    /* Verify part number.  A mismatch is only a warning: the device answered
     * at its address, and refusing to bind would hide the charge state. */
    uint8_t pn_reg = 0;
    int ret = bq25601_read(dev, BQ25601_REG0B, &pn_reg);
    if (ret < 0) {
        LOG_ERR("Failed to read part register: %d", ret);
        return ret;
    }
    uint8_t pn = (pn_reg & BQ25601_REG0B_PN_MASK) >> BQ25601_REG0B_PN_SHIFT;
    if (pn != BQ25601_PART_NUMBER) {
        LOG_WRN("Unexpected part number: 0x%02X (expected 0x%02X) — continuing",
                pn, BQ25601_PART_NUMBER);
    }

    /* Host mode with the I2C watchdog disabled.  Left at its 40 s default the
     * watchdog expires (nothing kicks it) and resets the charge current and
     * voltage below back to the chip defaults.  Termination stays enabled so
     * a full charge reports CHARGER_STATUS_FULL. */
    ret = bq25601_update(dev, BQ25601_REG05,
                         BQ25601_REG05_WATCHDOG_MASK | BQ25601_REG05_EN_TERM,
                         BQ25601_REG05_EN_TERM);
    if (ret < 0) {
        LOG_WRN("Failed to disable watchdog: %d", ret);
    }
    ret = bq25601_update(dev, BQ25601_REG01,
                         BQ25601_REG01_CHG_CONFIG | BQ25601_REG01_WD_RST,
                         BQ25601_REG01_CHG_CONFIG | BQ25601_REG01_WD_RST);
    if (ret < 0) {
        LOG_WRN("Failed to enable charging: %d", ret);
    }

    /* Apply initial charge current from DTS property */
    if (cfg->init_ichg_ua > 0) {
        union charger_propval v = { .const_charge_current_ua = cfg->init_ichg_ua };
        ret = bq25601_set_prop(dev, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &v);
        if (ret < 0) {
            LOG_WRN("Failed to set ICHG: %d", ret);
        }
    }

    /* Apply initial charge voltage from DTS property */
    if (cfg->init_vreg_uv > 0) {
        union charger_propval v = { .const_charge_voltage_uv = cfg->init_vreg_uv };
        ret = bq25601_set_prop(dev, CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV, &v);
        if (ret < 0) {
            LOG_WRN("Failed to set VREG: %d", ret);
        }
    }

    /* Optionally configure INT GPIO */
    if (cfg->int_gpio.port && gpio_is_ready_dt(&cfg->int_gpio)) {
        gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
    }

    LOG_INF("BQ25601 ready (PN=0x%02X)", pn);
    return 0;
}

/* =========================================================================
 * Driver API table
 * ========================================================================= */
static const struct charger_driver_api bq25601_api = {
    .get_property  = bq25601_get_prop,
    .set_property  = bq25601_set_prop,
    .charge_enable = bq25601_charge_enable,
};

/* =========================================================================
 * Device instantiation macro
 * ========================================================================= */
#define BQ25601_INIT(n)                                                        \
    static const struct bq25601_config bq25601_cfg_##n = {                    \
        .i2c         = I2C_DT_SPEC_INST_GET(n),                               \
        .int_gpio    = GPIO_DT_SPEC_INST_GET_OR(n, int_gpios, {0}),           \
        .init_ichg_ua = DT_INST_PROP_OR(n, constant_charge_current_max_microamp, 0), \
        .init_vreg_uv = DT_INST_PROP_OR(n, constant_charge_voltage_max_microvolt, 0),\
    };                                                                         \
    static struct bq25601_data bq25601_data_##n;                               \
    DEVICE_DT_INST_DEFINE(n, bq25601_init, NULL,                               \
                          &bq25601_data_##n, &bq25601_cfg_##n,                 \
                          POST_KERNEL, CONFIG_CHARGER_INIT_PRIORITY,           \
                          &bq25601_api);

DT_INST_FOREACH_STATUS_OKAY(BQ25601_INIT)
