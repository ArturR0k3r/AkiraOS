/**
 * @file st25dv.c
 * @brief ST25DV64K Dynamic NFC/RFID Tag I2C driver
 *
 * Two I2C devices are instantiated from DTS for the same physical chip:
 *   - DT node st25dv@53  → user memory (this driver's primary node)
 *   - DT node st25dv@57  → system memory (accessed via a second i2c_dt_spec)
 *
 * The driver obtains both addresses from device tree at init time.
 * Write page size is 4 bytes; writes crossing a page boundary are split.
 * Write cycle time is 5 ms max; the driver polls ACK before returning.
 */

#define DT_DRV_COMPAT st_st25dv64k

#include "st25dv.h"
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>
#ifdef CONFIG_AKIRA_NFC_MANAGER
#include <zephyr/init.h>
#include "connectivity/nfc_interface.h"
#endif

LOG_MODULE_REGISTER(st25dv, CONFIG_AKIRA_LOG_LEVEL);

/* =========================================================================
 * Register addresses (used with system I2C addr 0x57)
 * ========================================================================= */

/* Dynamic registers — accessible without password, volatile */
#define ST25DV_DYN_GPO_CTRL   0x2000  /* GPO interrupt source selection */
#define ST25DV_DYN_EH_CTRL    0x2002  /* Energy harvesting control */
#define ST25DV_DYN_RF_MNGT    0x2003  /* RF management */
#define ST25DV_DYN_I2C_SSO    0x2004  /* I2C security session open status */
#define ST25DV_DYN_IT_STS     0x2005  /* Interrupt status (read clears) */
#define ST25DV_DYN_MB_CTRL    0x2006  /* Mailbox control */
#define ST25DV_DYN_MB_LEN     0x2007  /* Mailbox message length */

/* Static config registers — need I2C security session to write */
#define ST25DV_SYS_GPO         0x0000  /* GPO configuration */
#define ST25DV_SYS_IT_TIME     0x0001  /* Interrupt pulse duration */
#define ST25DV_SYS_EH_MODE     0x0002  /* Energy harvesting mode */
#define ST25DV_SYS_RF_MNGT     0x0003  /* RF management static */
#define ST25DV_SYS_RFA1SS      0x0004  /* RF zone 1 security */
#define ST25DV_SYS_ENDA1       0x0005  /* RF zone 1 end address */
#define ST25DV_SYS_RFA2SS      0x0006
#define ST25DV_SYS_ENDA2       0x0007
#define ST25DV_SYS_RFA3SS      0x0008
#define ST25DV_SYS_ENDA3       0x0009
#define ST25DV_SYS_RFA4SS      0x000A
#define ST25DV_SYS_I2CSS       0x000B  /* I2C security session */
#define ST25DV_SYS_LOCK_CCFILE 0x000C  /* Lock CC file */
#define ST25DV_SYS_MB_MODE     0x000D  /* FTM auth + watchdog (Table 17) */
#define ST25DV_SYS_LOCK_CFG    0x000F  /* Lock configuration */
#define ST25DV_SYS_LOCK_DSFID  0x0010
#define ST25DV_SYS_LOCK_AFI    0x0011
#define ST25DV_SYS_DSFID       0x0012
#define ST25DV_SYS_AFI         0x0013
#define ST25DV_SYS_MEM_SIZE_MSB 0x0014  /* Memory size in 32-byte blocks */
#define ST25DV_SYS_MEM_SIZE_LSB 0x0015
#define ST25DV_SYS_BLK_SIZE    0x0016  /* Block size in bytes minus 1 */
#define ST25DV_SYS_IC_REF      0x0017  /* IC reference (should be 0x24) */
#define ST25DV_SYS_UID         0x0018  /* UID[63:0] — 8 bytes */
#define ST25DV_SYS_IC_REV      0x0020

/* I2C security session password register (system address, 2-byte addr) */
#define ST25DV_SYS_I2C_PWD     0x0900  /* Password: 8 bytes + validation byte */

/* DYN_I2C_SSO: session open flag */
#define ST25DV_I2C_SSO_OPEN    BIT(0)

/* DYN_EH_CTRL: RF field detect bit (datasheet Table 42, EH_CTRL_Dyn b2) */
#define ST25DV_RF_FIELD_ON     BIT(2)

/* Timing */
#define ST25DV_WRITE_CYCLE_MS  5   /* Max EEPROM write cycle time */
#define ST25DV_WRITE_PAGE_SZ   4   /* Write page size in bytes */
#define ST25DV_ACK_RETRIES     10  /* Poll attempts for ACK after write */

/* FTM register (0x000D) bitfield — Table 17 */
#define ST25DV_FTM_MB_MODE       BIT(0)  /* 1: enabling FTM authorized */
#define ST25DV_FTM_MB_WDG_SHIFT  1        /* watchdog = 2^(wdg-1) x 30ms, 0=infinite */

/* MB_CTRL_Dyn register (0x2006) bitfield — Table 19 */
#define ST25DV_MB_CTRL_EN               BIT(0)
#define ST25DV_MB_CTRL_HOST_PUT_MSG     BIT(1)
#define ST25DV_MB_CTRL_RF_PUT_MSG       BIT(2)
#define ST25DV_MB_CTRL_HOST_MISS_MSG    BIT(4)
#define ST25DV_MB_CTRL_RF_MISS_MSG      BIT(5)
#define ST25DV_MB_CTRL_HOST_CURRENT_MSG BIT(6)
#define ST25DV_MB_CTRL_RF_CURRENT_MSG   BIT(7)

/* FTM mailbox buffer base address (0x2008-0x2107), user I2C addr */
#define ST25DV_MB_BUF_BASE  0x2008

/* IC_REF (0x0017): ST25DV64KC-IE/JF = 0x51 (ST25DV04KC datasheet Table 84) */
#define ST25DV_IC_REF_EXPECTED 0x51

/* =========================================================================
 * Driver config / data
 * ========================================================================= */
struct st25dv_config {
    struct i2c_dt_spec  i2c_user;   /* reg     — user memory (default 0x53) */
    struct i2c_dt_spec  i2c_sys;    /* system-reg — sys config (default 0x57) */
    struct gpio_dt_spec gpo_gpio;   /* gpo-gpios — optional interrupt pin */
    uint8_t             gpo_sources; /* akira,gpo-sources bitmask */
};

struct st25dv_data {
    bool initialized;
    bool i2c_session_open;
};

/* =========================================================================
 * Low-level register I/O (handles 16-bit address prefix)
 * ========================================================================= */

static int st25dv_reg_read(const struct i2c_dt_spec *i2c,
                            uint16_t addr, uint8_t *data, size_t len)
{
    uint8_t addr_buf[2] = { addr >> 8, addr & 0xFF };
    return i2c_write_read_dt(i2c, addr_buf, sizeof(addr_buf), data, len);
}

static int st25dv_reg_write(const struct i2c_dt_spec *i2c,
                             uint16_t addr, const uint8_t *data, size_t len)
{
    /* Build [addr_hi][addr_lo][data...] in a single message */
    uint8_t buf[2 + ST25DV_MAX_XFER_LEN];
    if (len > ST25DV_MAX_XFER_LEN) {
        return -EINVAL;
    }
    buf[0] = addr >> 8;
    buf[1] = addr & 0xFF;
    memcpy(&buf[2], data, len);
    return i2c_write_dt(i2c, buf, 2 + len);
}

/* Poll for EEPROM write completion by checking ACK (NACK while busy). */
static int st25dv_wait_write_done(const struct i2c_dt_spec *i2c)
{
    for (int i = 0; i < ST25DV_ACK_RETRIES; i++) {
        k_msleep(1);
        uint8_t dummy = 0;
        if (st25dv_reg_read(i2c, 0x0000, &dummy, 1) == 0) {
            return 0;
        }
    }
    return -ETIMEDOUT;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int st25dv_read_user_mem(const struct device *dev,
                          uint16_t addr, uint8_t *buf, size_t len)
{
    const struct st25dv_config *cfg = dev->config;
    return st25dv_reg_read(&cfg->i2c_user, addr, buf, len);
}

int st25dv_write_user_mem(const struct device *dev,
                           uint16_t addr, const uint8_t *buf, size_t len)
{
    const struct st25dv_config *cfg = dev->config;
    size_t written = 0;

    while (written < len) {
        /* Stay within the current write page */
        uint16_t page_start  = (addr + written) & ~(ST25DV_WRITE_PAGE_SZ - 1);
        uint16_t page_remain = ST25DV_WRITE_PAGE_SZ
                               - ((addr + written) - page_start);
        size_t chunk = MIN(page_remain, len - written);

        int ret = st25dv_reg_write(&cfg->i2c_user, addr + written,
                                   buf + written, chunk);
        if (ret < 0) {
            return ret;
        }

        ret = st25dv_wait_write_done(&cfg->i2c_user);
        if (ret < 0) {
            return ret;
        }

        written += chunk;
    }
    return 0;
}

int st25dv_read_sys_reg(const struct device *dev, uint16_t addr, uint8_t *val)
{
    const struct st25dv_config *cfg = dev->config;
    return st25dv_reg_read(&cfg->i2c_sys, addr, val, 1);
}

int st25dv_write_sys_reg(const struct device *dev, uint16_t addr, uint8_t val)
{
    const struct st25dv_config *cfg = dev->config;
    int ret = st25dv_reg_write(&cfg->i2c_sys, addr, &val, 1);
    if (ret < 0) {
        return ret;
    }
    return st25dv_wait_write_done(&cfg->i2c_sys);
}

int st25dv_set_gpo(const struct device *dev, uint8_t sources)
{
    const struct st25dv_config *cfg = dev->config;
    /* Dynamic registers use device select E2=0,E1=1 — the user memory
     * address, not system (datasheet Table 14 / Table 89). */
    int ret = st25dv_reg_write(&cfg->i2c_user, ST25DV_DYN_GPO_CTRL, &sources, 1);
    if (ret < 0) {
        return ret;
    }
    return st25dv_wait_write_done(&cfg->i2c_user);
}

int st25dv_rf_field_present(const struct device *dev, bool *present)
{
    const struct st25dv_config *cfg = dev->config;
    uint8_t val = 0;
    int ret = st25dv_reg_read(&cfg->i2c_user, ST25DV_DYN_EH_CTRL, &val, 1);
    if (ret < 0) {
        return ret;
    }
    *present = (val & ST25DV_RF_FIELD_ON) != 0;
    return 0;
}

int st25dv_read_uid(const struct device *dev, uint8_t uid[8])
{
    const struct st25dv_config *cfg = dev->config;
    /* UID is stored MSB-first in 8 consecutive bytes starting at SYS_UID */
    int ret = st25dv_reg_read(&cfg->i2c_sys, ST25DV_SYS_UID, uid, 8);
    if (ret < 0) {
        return ret;
    }
    /* ST25DV stores UID little-endian per ISO 15693; reverse to network order */
    for (int i = 0; i < 4; i++) {
        uint8_t tmp = uid[i];
        uid[i] = uid[7 - i];
        uid[7 - i] = tmp;
    }
    return 0;
}

int st25dv_open_i2c_session(const struct device *dev, const uint8_t password[8])
{
    struct st25dv_data *data = dev->data;
    const struct st25dv_config *cfg = dev->config;

    /* Password presentation: write [password(8)] [0x09] [password(8)] to SYS_I2C_PWD */
    uint8_t buf[17];
    memcpy(buf, password, 8);
    buf[8] = 0x09; /* Validation byte */
    memcpy(&buf[9], password, 8);

    int ret = st25dv_reg_write(&cfg->i2c_sys, ST25DV_SYS_I2C_PWD, buf, sizeof(buf));
    if (ret < 0) {
        return ret;
    }
    k_msleep(1);

    /* Verify session opened. I2C_SSO_Dyn is a dynamic register — user
     * memory address, not system (datasheet Table 14 / Table 89). */
    uint8_t sso = 0;
    ret = st25dv_reg_read(&cfg->i2c_user, ST25DV_DYN_I2C_SSO, &sso, 1);
    if (ret < 0) {
        return ret;
    }
    if (!(sso & ST25DV_I2C_SSO_OPEN)) {
        LOG_WRN("I2C security session rejected");
        return -EACCES;
    }

    data->i2c_session_open = true;
    LOG_INF("ST25DV I2C security session opened");
    return 0;
}

int st25dv_mailbox_enable(const struct device *dev, bool enable, uint8_t wdg)
{
    const struct st25dv_config *cfg = dev->config;

    /* FTM (0x000D) is a static register — system address, requires an
     * open I2C security session (Table 16), same as other static regs. */
    uint8_t ftm = enable ? (ST25DV_FTM_MB_MODE | ((wdg & 0x7) << ST25DV_FTM_MB_WDG_SHIFT)) : 0;
    int ret = st25dv_write_sys_reg(dev, ST25DV_SYS_MB_MODE, ftm);
    if (ret < 0) {
        return ret;
    }

    /* MB_CTRL_Dyn is a dynamic register — user memory address, not system
     * (datasheet Table 14 / Table 89). */
    uint8_t mb_en = enable ? ST25DV_MB_CTRL_EN : 0;
    ret = st25dv_reg_write(&cfg->i2c_user, ST25DV_DYN_MB_CTRL, &mb_en, 1);
    if (ret < 0) {
        return ret;
    }
    return st25dv_wait_write_done(&cfg->i2c_user);
}

int st25dv_mailbox_put_msg(const struct device *dev, const uint8_t *buf, size_t len)
{
    const struct st25dv_config *cfg = dev->config;

    if (len == 0 || len > ST25DV_MAILBOX_MAX_LEN) {
        return -EINVAL;
    }
    /* Mailbox is a RAM buffer, not EEPROM — one-shot write, no page
     * splitting or write-cycle wait (datasheet: "write operation is done
     * immediately"). Must start at ST25DV_MB_BUF_BASE (no rollover). */
    return st25dv_reg_write(&cfg->i2c_user, ST25DV_MB_BUF_BASE, buf, len);
}

int st25dv_mailbox_get_msg(const struct device *dev, uint8_t *buf, size_t cap, size_t *len_out)
{
    const struct st25dv_config *cfg = dev->config;

    if (cap == 0) {
        return -EINVAL;
    }

    uint8_t len_reg = 0;
    int ret = st25dv_reg_read(&cfg->i2c_user, ST25DV_DYN_MB_LEN, &len_reg, 1);
    if (ret < 0) {
        return ret;
    }

    size_t len = (size_t)len_reg + 1;  /* MB_LEN_Dyn stores size minus 1 */
    if (len > cap) {
        len = cap;
    }

    ret = st25dv_reg_read(&cfg->i2c_user, ST25DV_MB_BUF_BASE, buf, len);
    if (ret < 0) {
        return ret;
    }

    *len_out = len;
    return 0;
}

int st25dv_mailbox_status(const struct device *dev, uint8_t *ctrl, size_t *msg_len)
{
    const struct st25dv_config *cfg = dev->config;

    int ret = st25dv_reg_read(&cfg->i2c_user, ST25DV_DYN_MB_CTRL, ctrl, 1);
    if (ret < 0) {
        return ret;
    }

    uint8_t len_reg = 0;
    ret = st25dv_reg_read(&cfg->i2c_user, ST25DV_DYN_MB_LEN, &len_reg, 1);
    if (ret < 0) {
        return ret;
    }
    *msg_len = (size_t)len_reg + 1;
    return 0;
}

/* =========================================================================
 * Init
 * ========================================================================= */
static int st25dv_init(const struct device *dev)
{
    const struct st25dv_config *cfg = dev->config;
    struct st25dv_data *data = dev->data;

    if (!i2c_is_ready_dt(&cfg->i2c_user)) {
        LOG_ERR("I2C user bus not ready");
        return -ENODEV;
    }
    if (!i2c_is_ready_dt(&cfg->i2c_sys)) {
        LOG_ERR("I2C sys bus not ready");
        return -ENODEV;
    }

    /* Verify IC reference */
    uint8_t ic_ref = 0;
    int ret = st25dv_reg_read(&cfg->i2c_sys, ST25DV_SYS_IC_REF, &ic_ref, 1);
    if (ret < 0) {
        LOG_ERR("IC reference read failed: %d", ret);
        return ret;
    }
    if (ic_ref != ST25DV_IC_REF_EXPECTED) {
        LOG_ERR("ST25DV not found (IC_REF=0x%02X, expected 0x%02X)",
                ic_ref, ST25DV_IC_REF_EXPECTED);
        return -ENODEV;
    }

    /* Read and log UID */
    uint8_t uid[8] = {0};
    if (st25dv_read_uid(dev, uid) == 0) {
        LOG_INF("ST25DV64K UID: %02X%02X%02X%02X%02X%02X%02X%02X",
                uid[0], uid[1], uid[2], uid[3],
                uid[4], uid[5], uid[6], uid[7]);
    }

    /* Configure GPO sources from DT (akira,gpo-sources).
     * Value 0x00 means "leave GPO unconfigured — application handles it". */
    if (cfg->gpo_sources != 0) {
        st25dv_set_gpo(dev, cfg->gpo_sources);
    }

    /* Configure GPO GPIO as input if provided */
    if (cfg->gpo_gpio.port && gpio_is_ready_dt(&cfg->gpo_gpio)) {
        gpio_pin_configure_dt(&cfg->gpo_gpio, GPIO_INPUT);
    }

    data->initialized = true;
    LOG_INF("ST25DV64K ready");
    return 0;
}

/* =========================================================================
 * Device instantiation
 * ========================================================================= */
/* system-reg DT property: secondary I2C address for system config space.
 * Default 0x57 if not specified — matches all standard ST25DV64K parts.
 * Override in DTS only when using a non-standard address variant. */
#define ST25DV_INIT(n)                                                          \
    static const struct st25dv_config st25dv_cfg_##n = {                       \
        .i2c_user    = I2C_DT_SPEC_INST_GET(n),                                \
        .i2c_sys = {                                                            \
            .bus  = DEVICE_DT_GET(DT_INST_BUS(n)),                             \
            /* Read system-reg from DTS; fall back to standard 0x57 */         \
            .addr = DT_INST_PROP_OR(n, system_reg, 0x57),                      \
        },                                                                      \
        .gpo_gpio    = GPIO_DT_SPEC_INST_GET_OR(n, gpo_gpios, {0}),            \
        /* akira,gpo-sources: bitmask from DTS; 0x89 = ENABLE|FIELDRISING|RFUSERWRITE */ \
        .gpo_sources = DT_INST_PROP_OR(n, akira_gpo_sources, 0x89),            \
    };                                                                          \
    static struct st25dv_data st25dv_data_##n;                                  \
    DEVICE_DT_INST_DEFINE(n, st25dv_init, NULL,                                 \
                          &st25dv_data_##n, &st25dv_cfg_##n,                    \
                          POST_KERNEL, 90,                                       \
                          NULL);  /* No standard Zephyr API for NFC EEPROM */

DT_INST_FOREACH_STATUS_OKAY(ST25DV_INIT)

/* =========================================================================
 * nfc_manager registration
 * ========================================================================= */
#ifdef CONFIG_AKIRA_NFC_MANAGER

static int st25dv_nfc_read_uid(nfc_handle_t *handle, uint8_t uid[8])
{
    return st25dv_read_uid((const struct device *)handle->priv_data, uid);
}

static int st25dv_nfc_read_mem(nfc_handle_t *handle, uint16_t addr, uint8_t *buf, size_t len)
{
    return st25dv_read_user_mem((const struct device *)handle->priv_data, addr, buf, len);
}

static int st25dv_nfc_write_mem(nfc_handle_t *handle, uint16_t addr, const uint8_t *buf, size_t len)
{
    return st25dv_write_user_mem((const struct device *)handle->priv_data, addr, buf, len);
}

static int st25dv_nfc_field_present(nfc_handle_t *handle, bool *present)
{
    return st25dv_rf_field_present((const struct device *)handle->priv_data, present);
}

static const nfc_ops_t st25dv_nfc_ops = {
    .read_uid      = st25dv_nfc_read_uid,
    .read_mem      = st25dv_nfc_read_mem,
    .write_mem     = st25dv_nfc_write_mem,
    .field_present = st25dv_nfc_field_present,
};

static nfc_handle_t st25dv_nfc_handle = {
    .type = NFC_TYPE_ST25DV,
    .name = "ST25DV",
    .ops  = &st25dv_nfc_ops,
};

static int st25dv_nfc_auto_register(void)
{
    const struct device *dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(st25dv));

    if (!dev || !device_is_ready(dev)) {
        LOG_WRN("st25dv not present or not ready — skipping nfc_manager registration");
        return 0;
    }

    st25dv_nfc_handle.priv_data = (void *)dev;
    int ret = nfc_manager_register(&st25dv_nfc_handle);
    if (ret < 0 && ret != -EALREADY) {
        LOG_ERR("Failed to register ST25DV: %d", ret);
        return ret;
    }
    LOG_INF("ST25DV registered with nfc_manager");
    return 0;
}

SYS_INIT(st25dv_nfc_auto_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_AKIRA_NFC_MANAGER */
