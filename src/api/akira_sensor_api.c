/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_sensor
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_sensor, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_sensor_api.c
 * @brief Generic sensor read API for WASM applications.
 *
 * Iterates all Zephyr static devices to find the first one that supports the
 * requested channel. No chip-specific code — any driver registered via DTS
 * and enabled with CONFIG_<DRIVER>=y is automatically accessible.
 *
 * WASM apps pass Zephyr enum sensor_channel integer values and receive the
 * result scaled ×1000 as int32 (callers divide by 1000.0 to recover float).
 */

#include "akira_api.h"
#include "akira_sensor_api.h"
#include <runtime/security.h>
#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <akira_app_containment.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <errno.h>

#ifdef CONFIG_SENSOR
#include <zephyr/drivers/sensor.h>

/*
 * Build a compile-time list of every DT-enabled sensor device.
 *
 * Why: iterating z_device_get_all_static() is unsafe — calling
 * sensor_sample_fetch_chan() on a non-sensor device (GPIO, I2C, …)
 * casts its driver API to sensor_driver_api* and jumps through a
 * wrong function pointer, causing a store-prohibited fault.
 *
 * DT_FOREACH_STATUS_OKAY expands to nothing if that compatible is
 * absent from the board DTS, so this list is always correct and
 * never needs manual maintenance.
 */
#define _SD(n) DEVICE_DT_GET(n),
#define _SD_COMPAT(compat) DT_FOREACH_STATUS_OKAY(compat, _SD)

static const struct device * const s_sensors[] = {
    /* ---- Inertial ---- */
    _SD_COMPAT(akira_lsm6ds3)
    _SD_COMPAT(st_lsm6dsl)
    _SD_COMPAT(st_lsm6dso)
    _SD_COMPAT(st_lsm6ds0)
    _SD_COMPAT(st_lsm9ds0_gyro)
    _SD_COMPAT(bosch_bmi270)
    _SD_COMPAT(bosch_bmi160)
    _SD_COMPAT(invensense_mpu6050)
    _SD_COMPAT(invensense_mpu9250)
    _SD_COMPAT(invensense_icm42688)
    /* ---- Environmental ---- */
    _SD_COMPAT(bosch_bme280)
    _SD_COMPAT(bosch_bmp280)
    _SD_COMPAT(sensirion_sht3xd)
    _SD_COMPAT(sensirion_sht4x)
    _SD_COMPAT(honeywell_hpma115s0)
    _SD_COMPAT(ams_ccs811)
    /* ---- Power / voltage ---- */
    _SD_COMPAT(ti_ina219)
    _SD_COMPAT(ti_ina226)
    _SD_COMPAT(ti_ina260)
    /* ---- Light ---- */
    _SD_COMPAT(ti_opt3001)
    _SD_COMPAT(maxim_max44009)
    _SD_COMPAT(rohm_bh1750)
    _SD_COMPAT(avago_apds9960)
    /* ---- Proximity / ToF ---- */
    _SD_COMPAT(st_vl53l0x)
    _SD_COMPAT(st_vl53l1x)
    NULL /* end marker: the list is empty on boards without sensors */
};
#undef _SD
#undef _SD_COMPAT

/* Bounded by the (compile-time) number of sensors; each fetch is one
 * driver transaction with the driver's own timeouts. */
int akira_sensor_read(int channel, float *out)
{
    if (!out) {
        return -EINVAL;
    }
    /* App-chosen: only the common channels.  SENSOR_CHAN_ALL makes some
     * drivers' channel_get() fill one value per channel, and private
     * channels have driver-defined result sizes. */
    if (channel < 0 || channel >= SENSOR_CHAN_ALL) {
        return -EINVAL;
    }
    enum sensor_channel ch = (enum sensor_channel)channel;

    for (size_t i = 0; s_sensors[i] != NULL; i++) {
        const struct device *dev = s_sensors[i];

        if (!device_is_ready(dev)) {
            /* Debug only: an app polling would repeat this every call. */
            LOG_DBG("sensor_read: device '%s' not ready (driver init failed?)", dev->name);
            continue;
        }

        int ret = sensor_sample_fetch_chan(dev, ch);
        if (ret < 0) {
            LOG_DBG("sensor_read: '%s' does not support channel=%d (ret=%d)", dev->name, channel, ret);
            continue;
        }

        /* Vector channels write several values: *_XYZ and POS_DXYZ three,
         * GAME_ROTATION_VECTOR four.  Room for all of them (a single
         * sensor_value here was overrun on the stack). */
        struct sensor_value val[4] = {0};
        ret = sensor_channel_get(dev, ch, val);
        if (ret < 0) {
            LOG_DBG("sensor_read: '%s' channel_get failed (channel=%d ret=%d)", dev->name, channel, ret);
            continue;
        }

        /* The API returns one scalar: the value of a scalar channel, the
         * first component of a vector channel (as before). */
        *out = (float)sensor_value_to_double(&val[0]);
        return 0;
    }

    LOG_DBG("sensor_read: no device answered channel=%d", channel);
    return -ENOTSUP;
}

#else /* CONFIG_SENSOR not set */

int akira_sensor_read(int channel, float *out)
{
    ARG_UNUSED(channel);
    ARG_UNUSED(out);
    return -ENOTSUP;
}

#endif /* CONFIG_SENSOR */

/* -------------------------------------------------------------------------- */
/* WASM native export                                                          */
/* -------------------------------------------------------------------------- */

#ifdef CONFIG_AKIRA_WASM_RUNTIME

int akira_native_sensor_read(wasm_exec_env_t exec_env, int32_t channel)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_SENSOR_READ, -EPERM);

    /* A fetch runs a transaction on the system bus with the driver's locks
     * held: a stop must not abort the app in the middle of it. */
    float v = 0.0f;
    akira_native_critical_enter(exec_env);
    int ret = akira_sensor_read((int)channel, &v);
    akira_native_critical_exit(exec_env);
    if (ret < 0) {
        LOG_DBG("sensor_read: channel=%d err=%d", (int)channel, ret);
        return INT32_MIN;
    }

    /* Scale ×1000 — WASM callers divide by 1000.0 to recover float.
     * Out-of-range (or NaN) values would make the conversion undefined:
     * saturate, keeping INT32_MIN as the error marker. */
    float scaled = v * 1000.0f;
    if (!(scaled == scaled)) {
        return INT32_MIN;
    }
    if (scaled >= 2147483520.0f) {
        return INT32_MAX;
    }
    if (scaled <= -2147483520.0f) {
        return INT32_MIN + 1;
    }
    return (int32_t)scaled;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_SENSOR))
#include <akira_native_registry.h>

static const NativeSymbol akira_sensor_natives[] = {
    {"sensor_read", (void *)akira_native_sensor_read, "(i)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_sensor_api, "env", akira_sensor_natives);
#endif
