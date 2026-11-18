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
 * bus_id 0 = i2c0 (platform-specific; may differ per board).
 *
 * System devices are off limits.  On AkiraConsole Prod, i2c0 is the system
 * bus (charger, IO expander driving chip selects/resets, fuel gauge, USB-PD,
 * backlight, secure element, IMU, BME280) AND the bus brought out on the J5
 * header for user sensors, so refusing the whole bus would kill the API's
 * purpose.  Instead every address the system uses is refused — reads too:
 * reads clear interrupt latches (FUSB302, TCA6408) and desync the SE050's
 * T=1 session.  Denied, before the bus is touched:
 *   - every child node of the bus in the devicetree, whatever its status (a
 *     disabled node is still soldered on), at its reg and system-reg,
 *   - the I2C reserved ranges 0x00-0x07 (general call = reset-all, CBUS,
 *     HS-mode) and 0x78-0x7F (10-bit, device ID),
 *   - on AkiraConsole Prod, parts fitted on i2c0 that have no node in the
 *     board devicetree (see I2C0_BOARD_LO/HI).
 * Capabilities are self-declared by the app, so this list — not the "i2c"
 * capability — is what protects the hardware.
 */

#include "akira_i2c_api.h"
#include <runtime/security.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <errno.h>
#include <string.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <akira_app_containment.h>

#define AKIRA_I2C_MAX_BUSES 2
#define AKIRA_I2C_MAX_BUF_LEN 256

static const char *const s_bus_names[] = {"i2c0", "i2c1"};
static const struct device *s_buses[AKIRA_I2C_MAX_BUSES];
static K_MUTEX_DEFINE(s_bus_lock);

/* -------------------------------------------------------------------------- */
/* System-address denylist (compile time, 128-bit: lo = 0x00-0x3F, hi = 0x40-0x7F) */
/* -------------------------------------------------------------------------- */

#define I2C_ADDR_LO(a) (((a) < 64) ? BIT64((a) & 63) : 0ULL)
#define I2C_ADDR_HI(a) (((a) >= 64 && (a) < 128) ? BIT64((a) & 63) : 0ULL)

/* Every address a child node answers on: reg (first two entries) and the
 * ST25DV-style second address "system-reg". */
#define I2C_CHILD_ADDRS(child, F)                                                        \
    COND_CODE_1(DT_REG_HAS_IDX(child, 0), (| F(DT_REG_ADDR_BY_IDX(child, 0))), ())       \
    COND_CODE_1(DT_REG_HAS_IDX(child, 1), (| F(DT_REG_ADDR_BY_IDX(child, 1))), ())       \
    COND_CODE_1(DT_NODE_HAS_PROP(child, system_reg), (| F(DT_PROP(child, system_reg))), ())
#define I2C_CHILD_LO(child) I2C_CHILD_ADDRS(child, I2C_ADDR_LO)
#define I2C_CHILD_HI(child) I2C_CHILD_ADDRS(child, I2C_ADDR_HI)

/* 0x00-0x07 and 0x78-0x7F are reserved by the I2C spec on every bus. */
#define I2C_RESERVED_LO GENMASK64(7, 0)
#define I2C_RESERVED_HI GENMASK64(63, 56)

#if defined(CONFIG_BOARD_AKIRACONSOLE_PROD)
/* Fitted on Prod's i2c0 (KiCad-verified part map) but absent from the board
 * devicetree, so the DT scan below cannot see them: LP5817 backlight driver
 * (0x2D) and its broadcast address (0x34), SE050 secure element (0x48, Rev
 * A.3), STC3115 fuel gauge (0x70). Harmless to list a part that later gains
 * a node. */
#define I2C0_BOARD_LO (I2C_ADDR_LO(0x2D) | I2C_ADDR_LO(0x34))
#define I2C0_BOARD_HI (I2C_ADDR_HI(0x48) | I2C_ADDR_HI(0x70))
#else
#define I2C0_BOARD_LO 0ULL
#define I2C0_BOARD_HI 0ULL
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(i2c0))
#define I2C0_DT_LO (0ULL DT_FOREACH_CHILD(DT_NODELABEL(i2c0), I2C_CHILD_LO))
#define I2C0_DT_HI (0ULL DT_FOREACH_CHILD(DT_NODELABEL(i2c0), I2C_CHILD_HI))
#else
#define I2C0_DT_LO 0ULL
#define I2C0_DT_HI 0ULL
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(i2c1))
#define I2C1_DT_LO (0ULL DT_FOREACH_CHILD(DT_NODELABEL(i2c1), I2C_CHILD_LO))
#define I2C1_DT_HI (0ULL DT_FOREACH_CHILD(DT_NODELABEL(i2c1), I2C_CHILD_HI))
#else
#define I2C1_DT_LO 0ULL
#define I2C1_DT_HI 0ULL
#endif

static const uint64_t s_deny_lo[AKIRA_I2C_MAX_BUSES] = {
    I2C_RESERVED_LO | I2C0_DT_LO | I2C0_BOARD_LO,
    I2C_RESERVED_LO | I2C1_DT_LO,
};
static const uint64_t s_deny_hi[AKIRA_I2C_MAX_BUSES] = {
    I2C_RESERVED_HI | I2C0_DT_HI | I2C0_BOARD_HI,
    I2C_RESERVED_HI | I2C1_DT_HI,
};

/* bus_id/dev_addr already range-checked by the caller. */
static bool i2c_addr_denied(int32_t bus_id, uint32_t dev_addr)
{
    return (dev_addr < 64) ? (s_deny_lo[bus_id] & BIT64(dev_addr)) != 0
                           : (s_deny_hi[bus_id] & BIT64(dev_addr - 64)) != 0;
}

/* Common argument checks, all done before any lock is taken. Failures are
 * the app's own doing (a scanner probes every address), so they log at
 * debug level only. */
static int i2c_check_args(const char *op, int32_t bus_id, int32_t dev_addr,
                          int32_t reg_addr, const uint8_t *buf, uint32_t len)
{
    if (!buf || len == 0 || len > AKIRA_I2C_MAX_BUF_LEN)
    {
        LOG_DBG("%s: invalid buf/len=%u", op, len);
        return -EINVAL;
    }
    if (bus_id < 0 || bus_id >= AKIRA_I2C_MAX_BUSES)
    {
        return -ENODEV;
    }
    if ((uint32_t)dev_addr > 0x7F)
    {
        LOG_DBG("%s: invalid dev_addr=0x%02x", op, dev_addr);
        return -EINVAL;
    }
    if ((uint32_t)reg_addr > 0xFF)
    {
        LOG_DBG("%s: invalid reg_addr=0x%02x", op, reg_addr);
        return -EINVAL;
    }
    if (i2c_addr_denied(bus_id, (uint32_t)dev_addr))
    {
        LOG_DBG("%s: bus %d addr 0x%02x is a system device, refused", op, bus_id,
                dev_addr);
        return -EACCES;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Lazy bus resolution                                                         */
/* -------------------------------------------------------------------------- */

/* Caller holds s_bus_lock. */
static const struct device *get_bus(int32_t bus_id)
{
    if (bus_id < 0 || bus_id >= AKIRA_I2C_MAX_BUSES)
    {
        return NULL;
    }
    if (s_buses[bus_id] != NULL)
    {
        return s_buses[bus_id];
    }
    const struct device *dev = device_get_by_dt_nodelabel(s_bus_names[bus_id]);
    if (!dev || !device_is_ready(dev))
    {
        LOG_DBG("i2c: bus %s not ready", s_bus_names[bus_id]);
        return NULL;
    }
    s_buses[bus_id] = dev;
    LOG_INF("i2c: bus %s resolved", s_bus_names[bus_id]);
    return dev;
}

/* -------------------------------------------------------------------------- */
/* WASM native exports                                                         */
/* -------------------------------------------------------------------------- */

/* Signature "(iii*~)i": WAMR validated buf..buf+len.  A transfer is at most
 * AKIRA_I2C_MAX_BUF_LEN bytes, so the bus is held for a bounded time (the
 * driver times out a stuck transfer); a stop never aborts the app while it
 * holds the bus (critical section). Nothing outlives the call. */
int akira_native_i2c_write_reg(wasm_exec_env_t exec_env,
                               int32_t bus_id, int32_t dev_addr, int32_t reg_addr,
                               const uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_I2C, -EPERM);

    int ret = i2c_check_args("i2c_write_reg", bus_id, dev_addr, reg_addr, buf, len);
    if (ret < 0)
    {
        return ret;
    }

    akira_native_critical_enter(exec_env);
    if (k_mutex_lock(&s_bus_lock, K_MSEC(500)) != 0)
    {
        akira_native_critical_exit(exec_env);
        LOG_DBG("i2c_write_reg: bus lock timed out");
        return -EBUSY;
    }
    const struct device *dev = get_bus(bus_id);
    ret = dev ? i2c_burst_write(dev, (uint16_t)dev_addr, (uint8_t)reg_addr, buf, len)
              : -ENODEV;
    k_mutex_unlock(&s_bus_lock);
    akira_native_critical_exit(exec_env);

    if (ret < 0 && ret != -ENODEV)
    {
        LOG_DBG("i2c_write_reg: bus=%d addr=0x%02x reg=0x%02x err=%d",
                bus_id, dev_addr, reg_addr, ret);
    }
    return ret;
}

int akira_native_i2c_read_reg(wasm_exec_env_t exec_env,
                              int32_t bus_id, int32_t dev_addr, int32_t reg_addr,
                              uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_I2C, -EPERM);

    int ret = i2c_check_args("i2c_read_reg", bus_id, dev_addr, reg_addr, buf, len);
    if (ret < 0)
    {
        return ret;
    }

    akira_native_critical_enter(exec_env);
    if (k_mutex_lock(&s_bus_lock, K_MSEC(500)) != 0)
    {
        akira_native_critical_exit(exec_env);
        LOG_DBG("i2c_read_reg: bus lock timed out");
        return -EBUSY;
    }
    const struct device *dev = get_bus(bus_id);
    ret = dev ? i2c_burst_read(dev, (uint16_t)dev_addr, (uint8_t)reg_addr, buf, len)
              : -ENODEV;
    k_mutex_unlock(&s_bus_lock);
    akira_native_critical_exit(exec_env);

    if (ret < 0)
    {
        if (ret != -ENODEV)
        {
            LOG_DBG("i2c_read_reg: bus=%d addr=0x%02x reg=0x%02x err=%d",
                    bus_id, dev_addr, reg_addr, ret);
        }
        return ret;
    }
    return (int)len;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_I2C))
#include <akira_native_registry.h>

static const NativeSymbol akira_i2c_natives[] = {
    {"i2c_write_reg", (void *)akira_native_i2c_write_reg, "(iii*~)i", NULL},
    {"i2c_read_reg", (void *)akira_native_i2c_read_reg, "(iii*~)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_i2c_api, "env", akira_i2c_natives);
#endif
