/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
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

/* Boot-time init: a transient bus fault must not leave the console dark. */
#define LP5817_BOOT_ATTEMPTS    3
#define LP5817_RETRY_DELAY_MS   5

static const struct device *const i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));

/* Serialises every register sequence: display_hal (main thread during
 * bring-up), the OS shell, the wait screen and the `display brightness`
 * command all call in, and an interleaved brightness-0 sequence could
 * otherwise clear CHIP_EN right after another caller lit the panel. */
static K_MUTEX_DEFINE(lp5817_lock);

static bool lp5817_ready;
/* display_hal has made its first init call.  Until then the panel GRAM may
 * still hold garbage, so brightness requests must not light it. */
static bool lp5817_init_attempted;

static int lp5817_write(uint8_t reg, uint8_t val)
{
    return i2c_reg_write_byte(i2c_dev, LP5817_I2C_ADDR, reg, val);
}

bool akira_lp5817_is_ready(void)
{
    return lp5817_ready;
}

/* Chip on, all three strings on, full-scale dot current; brightness is then
 * the 8-bit PWM alone.  Repeated before every non-zero brightness: five
 * writes, and it recovers an LP5817 that lost its registers (brown-out)
 * while the bus kept ACKing.  DEV_CONFIG1 is latched by the caller's
 * UPDATE_CMD. */
static int lp5817_enable(void)
{
    int ret = lp5817_write(LP5817_REG_CHIP_EN, LP5817_CHIP_EN);

    if (ret == 0) {
        ret = lp5817_write(LP5817_REG_DEV_CONFIG1, LP5817_OUT_ALL);
    }
    for (uint8_t ch = 0; ret == 0 && ch < LP5817_NUM_CHANNELS; ch++) {
        ret = lp5817_write(LP5817_REG_OUT0_DC + ch, 0xFF);
    }
    return ret;
}

static int lp5817_write_pwm(uint8_t brightness)
{
    int ret = 0;

    for (uint8_t ch = 0; ret == 0 && ch < LP5817_NUM_CHANNELS; ch++) {
        ret = lp5817_write(LP5817_REG_OUT0_PWM + ch, brightness);
    }
    if (ret == 0) {
        ret = lp5817_write(LP5817_REG_UPDATE_CMD, LP5817_CMD_UPDATE);
    }
    return ret;
}

/* Reset to a known state and leave the device enabled but dark. */
static int lp5817_configure(void)
{
    /* Probe first: the device must ACK before any later write is trusted. */
    int ret = lp5817_write(LP5817_REG_CHIP_EN, LP5817_CHIP_EN);

    if (ret < 0) {
        LOG_DBG("no ACK at 0x%02x: %d", LP5817_I2C_ADDR, ret);
        return ret;
    }

    ret = lp5817_write(LP5817_REG_RESET_CMD, LP5817_CMD_RESET);
    if (ret < 0) {
        LOG_DBG("reset failed: %d", ret);
        return ret;
    }
    k_msleep(LP5817_RESET_DELAY_MS);

    /* RESET cleared CHIP_EN and every output setting. */
    ret = lp5817_enable();
    if (ret < 0) {
        LOG_DBG("enable failed: %d", ret);
        return ret;
    }

    /* Start dark; display_hal raises brightness once the panel is ready, so
     * the user never sees GRAM garbage lit up during initialisation. */
    ret = lp5817_write_pwm(0);
    if (ret < 0) {
        LOG_DBG("PWM clear failed: %d", ret);
    }
    return ret;
}

static int lp5817_init_locked(int attempts)
{
    int ret = -EIO;

    for (int i = 0; i < attempts; i++) {
        if (i > 0) {
            k_msleep(LP5817_RETRY_DELAY_MS);
        }
        ret = lp5817_configure();
        if (ret == 0) {
            lp5817_ready = true;
            LOG_INF("LP5817 backlight ready (I2C 0x%02x, 3 channels%s)",
                    LP5817_I2C_ADDR, i > 0 ? ", after retry" : "");
            return 0;
        }
    }
    LOG_ERR("LP5817 not responding at 0x%02x: %d (retried on the next brightness change)",
            LP5817_I2C_ADDR, ret);
    return ret;
}

int akira_lp5817_set_brightness(uint8_t brightness)
{
    int ret = 0;

    k_mutex_lock(&lp5817_lock, K_FOREVER);

    if (!lp5817_ready) {
        /* Once display_hal has tried, every brightness change retries
         * (one attempt: a dead bus costs a timeout per transfer). */
        ret = lp5817_init_attempted ? lp5817_init_locked(1) : -ENODEV;
    }
    if (ret == 0 && brightness != 0) {
        ret = lp5817_enable();
    }
    if (ret == 0) {
        ret = lp5817_write_pwm(brightness);
    }
    /* 0 = standby: with every PWM at zero the strings are dark anyway, but
     * an enabled LP5817 still burns ~0.3-0.5 mA in its oscillator and
     * references, on the wait screen and through deep sleep. */
    if (ret == 0 && brightness == 0) {
        ret = lp5817_write(LP5817_REG_CHIP_EN, 0);
    }
    if (ret < 0 && ret != -ENODEV) {
        LOG_ERR("LP5817 brightness %u failed: %d", brightness, ret);
    }

    k_mutex_unlock(&lp5817_lock);
    return ret;
}

int akira_lp5817_init(void)
{
    int ret;

    if (!device_is_ready(i2c_dev)) {
        LOG_ERR("I2C bus not ready — backlight unavailable");
        return -ENODEV;
    }

    k_mutex_lock(&lp5817_lock, K_FOREVER);
    lp5817_init_attempted = true;
    ret = lp5817_ready ? 0 : lp5817_init_locked(LP5817_BOOT_ATTEMPTS);
    k_mutex_unlock(&lp5817_lock);
    return ret;
}
