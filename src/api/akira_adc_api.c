/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_adc
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_adc, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_adc_api.c
 * @brief ADC channel read API for WASM applications.
 *
 * Hardware-agnostic: the ADC device is resolved from the DT alias "akira-adc"
 * when defined; otherwise the first zephyr,adc device is used.  All channel
 * parameters (gain, reference voltage, resolution) are set via Kconfig so
 * board-specific tuning lives entirely in .conf and .overlay files.
 *
 * Overlay example (board-specific):
 *   / { aliases { akira-adc = &adc1; }; };
 *   &adc1 { status = "okay"; };
 *
 * Conf example:
 *   CONFIG_ADC=y
 *   CONFIG_AKIRA_WASM_ADC=y
 *   CONFIG_AKIRA_WASM_ADC_VREF_MV=3300
 *   CONFIG_AKIRA_WASM_ADC_RESOLUTION=12
 *   CONFIG_AKIRA_WASM_ADC_CHANNEL_MASK=<channels wired to analog inputs>
 *
 * ESP32-S3 note: in Zephyr's devicetree adc0 is SAR ADC1 (unit 1, CHn =
 * GPIO(n+1)) and adc1 is SAR ADC2 (unit 2, CHn = GPIO(n+11)).
 */

#include "akira_adc_api.h"
#include <runtime/security.h>
#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <akira_app_containment.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <errno.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Device resolution                                                           */
/* -------------------------------------------------------------------------- */

#if DT_NODE_EXISTS(DT_ALIAS(akira_adc))
#define AKIRA_ADC_DEV_INIT() DEVICE_DT_GET(DT_ALIAS(akira_adc))
#else
#define AKIRA_ADC_DEV_INIT() DEVICE_DT_GET_ANY(zephyr_adc)
#endif

/* -------------------------------------------------------------------------- */
/* Channel policy                                                              */
/* -------------------------------------------------------------------------- */

/* Setting up an ADC channel turns its pad into an analog input — on ESP32 the
 * driver calls gpio_pin_configure(pad, GPIO_DISCONNECTED).  If that pad is a
 * bus or chip-select line, the first adc_read() takes the display, SD card or
 * radios off the bus.  So a channel is usable only if:
 *   - its bit is set in CONFIG_AKIRA_WASM_ADC_CHANNEL_MASK (per-board
 *     allowlist; boards list exactly the channels wired to analog inputs), and
 *   - on ESP32, the pad it maps to is not a system pin (devicetree GPIO /
 *     pinctrl user, flash/PSRAM, USB, UART0, strapping) — checked against the
 *     GPIO API's policy as a second, board-independent line of defence.  The
 *     pad comes from the same table the driver uses (adc_channel_io_map). */
#ifndef CONFIG_AKIRA_WASM_ADC_CHANNEL_MASK
#define CONFIG_AKIRA_WASM_ADC_CHANNEL_MASK 0xFFFF
#endif

#if defined(CONFIG_ADC_ESP32) && defined(CONFIG_GPIO) && \
    DT_NODE_EXISTS(DT_ALIAS(akira_adc)) && DT_NODE_HAS_PROP(DT_ALIAS(akira_adc), unit)
#include "akira_gpio_api.h"
#include <soc/soc_caps.h>
#include <soc/adc_periph.h>
/* DT "unit" is 1-based; the HAL table is indexed by adc_unit_t (0-based). */
#define AKIRA_ADC_UNIT_IDX (DT_PROP(DT_ALIAS(akira_adc), unit) - 1)
BUILD_ASSERT(AKIRA_ADC_UNIT_IDX >= 0 && AKIRA_ADC_UNIT_IDX < SOC_ADC_PERIPH_NUM,
             "akira-adc unit out of range");

static bool adc_channel_pad_is_system(int ch)
{
    if (ch >= SOC_ADC_MAX_CHANNEL_NUM) {
        return true;
    }
    int pad = adc_channel_io_map[AKIRA_ADC_UNIT_IDX][ch];

    /* No pad: the driver refuses the channel itself; refuse it here too. */
    return pad < 0 || akira_gpio_pin_is_system((uint32_t)pad);
}
#else
/* No pad table for this ADC (or no GPIO policy): only the board mask
 * decides, so boards must keep CONFIG_AKIRA_WASM_ADC_CHANNEL_MASK exact. */
static bool adc_channel_pad_is_system(int ch)
{
    ARG_UNUSED(ch);
    return false;
}
#endif

static bool adc_channel_allowed(int ch)
{
    return ch >= 0 && ch < CONFIG_AKIRA_WASM_ADC_MAX_CHANNELS && ch < 32 &&
           (((uint32_t)CONFIG_AKIRA_WASM_ADC_CHANNEL_MASK >> ch) & 1U) != 0 &&
           !adc_channel_pad_is_system(ch);
}

static const struct device *s_adc_dev;
static bool s_ch_ready[CONFIG_AKIRA_WASM_ADC_MAX_CHANNELS];
static bool s_initialized;
static bool s_init_failed_logged;

/* The ESP32 ADC driver has no lock of its own: serialise channel setup and
 * conversions between apps.  Bounded wait: a caller never queues forever. */
static K_MUTEX_DEFINE(s_adc_lock);
#define AKIRA_ADC_LOCK_TIMEOUT K_MSEC(200)

static int akira_adc_init(void)
{
    if (s_initialized) {
        return 0;
    }

    s_adc_dev = AKIRA_ADC_DEV_INIT();
    if (!s_adc_dev || !device_is_ready(s_adc_dev)) {
        /* Once: a polling app would otherwise log this on every call. */
        if (!s_init_failed_logged) {
            s_init_failed_logged = true;
            LOG_ERR("ADC device not ready");
        }
        s_adc_dev = NULL;
        return -ENODEV;
    }

    LOG_INF("ADC device ready: %s (vref=%d mV, res=%d bit, max_ch=%d, mask=0x%x)",
            s_adc_dev->name,
            CONFIG_AKIRA_WASM_ADC_VREF_MV,
            CONFIG_AKIRA_WASM_ADC_RESOLUTION,
            CONFIG_AKIRA_WASM_ADC_MAX_CHANNELS,
            (unsigned int)CONFIG_AKIRA_WASM_ADC_CHANNEL_MASK);

    s_initialized = true;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Per-channel lazy setup                                                      */
/* -------------------------------------------------------------------------- */

static int ensure_channel(int ch)
{
    if (s_ch_ready[ch]) {
        return 0;
    }

    struct adc_channel_cfg cfg = {
        .gain             = ADC_GAIN_1,
        .reference        = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME_DEFAULT,
        .channel_id       = (uint8_t)ch,
        .differential     = 0,
    };

    int ret = adc_channel_setup(s_adc_dev, &cfg);
    if (ret < 0) {
        LOG_DBG("ADC channel %d setup failed: %d", ch, ret);
        return ret;
    }

    s_ch_ready[ch] = true;
    LOG_DBG("ADC channel %d configured", ch);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Core API (usable without WASM runtime)                                     */
/* -------------------------------------------------------------------------- */

int akira_adc_read(int channel)
{
    if (channel < 0 || channel >= CONFIG_AKIRA_WASM_ADC_MAX_CHANNELS) {
        return -EINVAL;
    }
    /* Before any driver call: never let channel setup claim a system pad. */
    if (!adc_channel_allowed(channel)) {
        return -EACCES;
    }
    if (k_mutex_lock(&s_adc_lock, AKIRA_ADC_LOCK_TIMEOUT) != 0) {
        return -EBUSY;
    }
    if (akira_adc_init() != 0) {
        k_mutex_unlock(&s_adc_lock);
        return -ENODEV;
    }

    int ret = ensure_channel(channel);
    if (ret < 0) {
        k_mutex_unlock(&s_adc_lock);
        return ret;
    }

    int16_t buf = 0;
    struct adc_sequence seq = {
        .channels    = BIT(channel),
        .buffer      = &buf,
        .buffer_size = sizeof(buf),
        .resolution  = CONFIG_AKIRA_WASM_ADC_RESOLUTION,
    };

    ret = adc_read(s_adc_dev, &seq);
    k_mutex_unlock(&s_adc_lock);
    if (ret < 0) {
        LOG_DBG("ADC read ch=%d err=%d", channel, ret);
        return ret;
    }

    LOG_DBG("ADC ch=%d raw=%d", channel, buf);
    return (int)buf;
}

int akira_adc_read_mv(int channel)
{
    int raw = akira_adc_read(channel);
    if (raw < 0) {
        return raw;
    }

    int32_t mv = raw;
    int ret = adc_raw_to_millivolts(CONFIG_AKIRA_WASM_ADC_VREF_MV,
                                    ADC_GAIN_1,
                                    CONFIG_AKIRA_WASM_ADC_RESOLUTION,
                                    &mv);
    if (ret < 0) {
        LOG_DBG("ADC raw_to_mv ch=%d err=%d", channel, ret);
        return ret;
    }

    LOG_DBG("ADC ch=%d raw=%d mv=%d", channel, raw, mv);
    return (int)mv;
}

/* -------------------------------------------------------------------------- */
/* WASM native exports                                                         */
/* -------------------------------------------------------------------------- */

#ifdef CONFIG_AKIRA_WASM_RUNTIME

/* A read holds s_adc_lock for one bounded conversion: a stop must not abort
 * the app inside it. Nothing outlives the call, so no release hook. */
int akira_native_adc_read(wasm_exec_env_t exec_env, int32_t channel)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_ADC, -EPERM);
    akira_native_critical_enter(exec_env);
    int ret = akira_adc_read((int)channel);
    akira_native_critical_exit(exec_env);
    return ret;
}

int akira_native_adc_read_mv(wasm_exec_env_t exec_env, int32_t channel)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_ADC, -EPERM);
    akira_native_critical_enter(exec_env);
    int ret = akira_adc_read_mv((int)channel);
    akira_native_critical_exit(exec_env);
    return ret;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_ADC))
#include <akira_native_registry.h>

static const NativeSymbol akira_adc_natives[] = {
    {"adc_read", (void *)akira_native_adc_read, "(i)i", NULL},
    {"adc_read_mv", (void *)akira_native_adc_read_mv, "(i)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_adc_api, "env", akira_adc_natives);
#endif
