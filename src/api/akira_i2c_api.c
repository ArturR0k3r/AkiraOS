/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_i2c
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_i2c, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_i2c_api.c
 * @brief Stateless raw I2C register read/write API for WASM applications
 *
 * Buses are resolved lazily on first access and cached.
 * bus_id 0 = i2c0 (where LSM6DS3 lives on akiraconsole).
 */

#include "akira_i2c_api.h"
#include <runtime/security.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <errno.h>
#include <string.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#define AKIRA_I2C_MAX_BUSES   2
#define AKIRA_I2C_MAX_BUF_LEN 256

static const char *const s_bus_names[] = { "i2c0", "i2c1" };
static const struct device *s_buses[AKIRA_I2C_MAX_BUSES];
static K_MUTEX_DEFINE(s_bus_lock);

/* -------------------------------------------------------------------------- */
/* Lazy bus resolution                                                         */
/* -------------------------------------------------------------------------- */

static const struct device *get_bus(int32_t bus_id)
{
    if (bus_id < 0 || bus_id >= AKIRA_I2C_MAX_BUSES) {
        return NULL;
    }
    if (s_buses[bus_id] != NULL) {
        return s_buses[bus_id];
    }
    const struct device *dev = device_get_binding(s_bus_names[bus_id]);
    if (!dev || !device_is_ready(dev)) {
        LOG_ERR("i2c: bus %s not ready", s_bus_names[bus_id]);
        return NULL;
    }
    s_buses[bus_id] = dev;
    LOG_INF("i2c: bus %s resolved", s_bus_names[bus_id]);
    return dev;
}

/* -------------------------------------------------------------------------- */
/* WASM native exports                                                         */
/* -------------------------------------------------------------------------- */

int akira_native_i2c_write_reg(wasm_exec_env_t exec_env,
                                int32_t bus_id, int32_t dev_addr, int32_t reg_addr,
                                const uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_I2C, -EPERM);

    if (!buf || len == 0 || len > AKIRA_I2C_MAX_BUF_LEN) {
        LOG_ERR("i2c_write_reg: invalid buf/len=%u", len);
        return -EINVAL;
    }
    if ((uint32_t)dev_addr > 0x7F) {
        LOG_ERR("i2c_write_reg: invalid dev_addr=0x%02x", dev_addr);
        return -EINVAL;
    }
    if ((uint32_t)reg_addr > 0xFF) {
        LOG_ERR("i2c_write_reg: invalid reg_addr=0x%02x", reg_addr);
        return -EINVAL;
    }

    k_mutex_lock(&s_bus_lock, K_FOREVER);
    const struct device *dev = get_bus(bus_id);
    if (!dev) {
        k_mutex_unlock(&s_bus_lock);
        return -ENODEV;
    }

    int ret = i2c_burst_write(dev, (uint16_t)dev_addr, (uint8_t)reg_addr, buf, len);
    k_mutex_unlock(&s_bus_lock);

    if (ret < 0) {
        LOG_ERR("i2c_write_reg: bus=%d addr=0x%02x reg=0x%02x err=%d",
                bus_id, dev_addr, reg_addr, ret);
    }
    return ret;
}

int akira_native_i2c_read_reg(wasm_exec_env_t exec_env,
                               int32_t bus_id, int32_t dev_addr, int32_t reg_addr,
                               uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_I2C, -EPERM);

    if (!buf || len == 0 || len > AKIRA_I2C_MAX_BUF_LEN) {
        LOG_ERR("i2c_read_reg: invalid buf/len=%u", len);
        return -EINVAL;
    }
    if ((uint32_t)dev_addr > 0x7F) {
        LOG_ERR("i2c_read_reg: invalid dev_addr=0x%02x", dev_addr);
        return -EINVAL;
    }
    if ((uint32_t)reg_addr > 0xFF) {
        LOG_ERR("i2c_read_reg: invalid reg_addr=0x%02x", reg_addr);
        return -EINVAL;
    }

    k_mutex_lock(&s_bus_lock, K_FOREVER);
    const struct device *dev = get_bus(bus_id);
    if (!dev) {
        k_mutex_unlock(&s_bus_lock);
        return -ENODEV;
    }

    int ret = i2c_burst_read(dev, (uint16_t)dev_addr, (uint8_t)reg_addr, buf, len);
    k_mutex_unlock(&s_bus_lock);

    if (ret < 0) {
        LOG_ERR("i2c_read_reg: bus=%d addr=0x%02x reg=0x%02x err=%d",
                bus_id, dev_addr, reg_addr, ret);
        return ret;
    }
    return (int)len;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
