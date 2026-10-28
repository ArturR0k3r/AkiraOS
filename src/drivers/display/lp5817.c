/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file lp5817.c
 * @brief TI LP5817 3-channel I2C LED driver — AkiraConsole display backlight.
 *
 * See lp5817.h for the hardware wiring.  Register map and command magic
 * values are from the LP5817 datasheet (SLVSGx / ti.com/lit/gpn/lp5817).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include "lp5817.h"

LOG_MODULE_REGISTER(lp5817, CONFIG_AKIRA_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Register map                                                        */
/* ------------------------------------------------------------------ */
#define LP5817_REG_CHIP_EN      0x00 /* bit0: 1 = enable device            */
#define LP5817_REG_DEV_CONFIG0  0x01 /* max-current range select           */
#define LP5817_REG_DEV_CONFIG1  0x02 /* per-channel output enable          */
#define LP5817_REG_DEV_CONFIG2  0x03 /* fade enable + fade time            */
#define LP5817_REG_DEV_CONFIG3  0x04 /* exponential dimming enable         */
#define LP5817_REG_RESET_CMD    0x0E /* write 0xCC to reset all registers  */
#define LP5817_REG_UPDATE_CMD   0x0F /* write 0x55 to latch configuration  */
#define LP5817_REG_OUT0_DC      0x14 /* analog dot-current, per channel    */
#define LP5817_REG_OUT0_PWM     0x18 /* 8-bit manual PWM, per channel      */

#define LP5817_CMD_RESET        0xCC
#define LP5817_CMD_UPDATE       0x55

#define LP5817_CHIP_EN          0x01
#define LP5817_OUT_ALL          0x07 /* OUT0 | OUT1 | OUT2                 */
#define LP5817_NUM_CHANNELS     3

/* The datasheet specifies the device address as 0x2D, with 0x34 acting as a
 * broadcast address that every LP5817 on the bus also acknowledges.  Only one
 * device is fitted, so the dedicated address is used. */
#define LP5817_I2C_ADDR         0x2D

/* Reset needs a short settling period before the device accepts writes. */
#define LP5817_RESET_DELAY_MS   2

static const struct device *i2c_dev;
static bool lp5817_ready;

static int lp5817_write(uint8_t reg, uint8_t val)
{
    return i2c_reg_write_byte(i2c_dev, LP5817_I2C_ADDR, reg, val);
}

bool akira_lp5817_is_ready(void)
{
    return lp5817_ready;
}

/* CHIP_EN currently set.  Brightness 0 clears it: with every PWM at zero the
 * strings are dark anyway, but an enabled LP5817 still burns ~0.3-0.5 mA in
 * its oscillator and references — on the wait screen and through deep sleep. */
static bool lp5817_enabled;

/* Bring the device back from standby.  Registers survive CHIP_EN=0, but the
 * output/current setup is cheap to repeat and makes this robust against a
 * brown-out reset of the LP5817 alone. */
static int lp5817_enable(void)
{
    int ret = lp5817_write(LP5817_REG_CHIP_EN, LP5817_CHIP_EN);

    if (ret == 0) {
        ret = lp5817_write(LP5817_REG_DEV_CONFIG1, LP5817_OUT_ALL);
    }
    for (uint8_t ch = 0; ret == 0 && ch < LP5817_NUM_CHANNELS; ch++) {
        ret = lp5817_write(LP5817_REG_OUT0_DC + ch, 0xFF);
    }
    if (ret == 0) {
        lp5817_enabled = true;
    }
    return ret;
}

int akira_lp5817_set_brightness(uint8_t brightness)
{
    if (!lp5817_ready) {
        return -ENODEV;
    }

    if (brightness != 0 && !lp5817_enabled) {
        int ret = lp5817_enable();

        if (ret < 0) {
            LOG_ERR("LP5817 re-enable failed: %d", ret);
            return ret;
        }
    }

    for (uint8_t ch = 0; ch < LP5817_NUM_CHANNELS; ch++) {
        int ret = lp5817_write(LP5817_REG_OUT0_PWM + ch, brightness);
        if (ret < 0) {
            LOG_ERR("PWM write ch%u failed: %d", ch, ret);
            return ret;
        }
    }

    /* Latch the new PWM values. */
    int ret = lp5817_write(LP5817_REG_UPDATE_CMD, LP5817_CMD_UPDATE);

    if (ret == 0 && brightness == 0 && lp5817_enabled) {
        ret = lp5817_write(LP5817_REG_CHIP_EN, 0);
        if (ret == 0) {
            lp5817_enabled = false;
        }
    }
    return ret;
}

int akira_lp5817_init(void)
{
    int ret;

    i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
    if (!device_is_ready(i2c_dev)) {
        LOG_ERR("I2C bus not ready — backlight unavailable");
        return -ENODEV;
    }

    /* Probe: the device must ACK before we trust any later write. */
    ret = lp5817_write(LP5817_REG_CHIP_EN, LP5817_CHIP_EN);
    if (ret < 0) {
        LOG_ERR("LP5817 not responding at 0x%02x: %d", LP5817_I2C_ADDR, ret);
        return ret;
    }

    /* Reset to a known state, then re-enable (RESET clears CHIP_EN). */
    ret = lp5817_write(LP5817_REG_RESET_CMD, LP5817_CMD_RESET);
    if (ret < 0) {
        LOG_ERR("LP5817 reset failed: %d", ret);
        return ret;
    }
    k_msleep(LP5817_RESET_DELAY_MS);

    ret = lp5817_write(LP5817_REG_CHIP_EN, LP5817_CHIP_EN);
    if (ret < 0) {
        LOG_ERR("LP5817 enable failed: %d", ret);
        return ret;
    }

    /* Enable all three LED strings. */
    ret = lp5817_write(LP5817_REG_DEV_CONFIG1, LP5817_OUT_ALL);
    if (ret < 0) {
        LOG_ERR("LP5817 output enable failed: %d", ret);
        return ret;
    }

    /* Full-scale analog dot-current on every channel; brightness is then
     * controlled purely by the 8-bit PWM registers. */
    for (uint8_t ch = 0; ch < LP5817_NUM_CHANNELS; ch++) {
        ret = lp5817_write(LP5817_REG_OUT0_DC + ch, 0xFF);
        if (ret < 0) {
            LOG_ERR("LP5817 dot-current ch%u failed: %d", ch, ret);
            return ret;
        }
    }

    /* Start dark; display_hal raises brightness once the panel is ready, so
     * the user never sees GRAM garbage lit up during initialisation. */
    for (uint8_t ch = 0; ch < LP5817_NUM_CHANNELS; ch++) {
        ret = lp5817_write(LP5817_REG_OUT0_PWM + ch, 0x00);
        if (ret < 0) {
            LOG_ERR("LP5817 PWM clear ch%u failed: %d", ch, ret);
            return ret;
        }
    }

    ret = lp5817_write(LP5817_REG_UPDATE_CMD, LP5817_CMD_UPDATE);
    if (ret < 0) {
        LOG_ERR("LP5817 update failed: %d", ret);
        return ret;
    }

    lp5817_enabled = true;
    lp5817_ready = true;
    LOG_INF("LP5817 backlight ready (I2C 0x%02x, 3 channels)", LP5817_I2C_ADDR);
    return 0;
}
