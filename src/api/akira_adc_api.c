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
 */

#include "akira_adc_api.h"
#include <runtime/security.h>
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

static const struct device *s_adc_dev;
static bool s_ch_ready[CONFIG_AKIRA_WASM_ADC_MAX_CHANNELS];
static bool s_initialized;

static int akira_adc_init(void)
{
    if (s_initialized) {
        return 0;
    }

    s_adc_dev = AKIRA_ADC_DEV_INIT();
    if (!s_adc_dev || !device_is_ready(s_adc_dev)) {
        LOG_ERR("ADC device not ready");
        s_adc_dev = NULL;
        return -ENODEV;
    }

    LOG_INF("ADC device ready: %s (vref=%d mV, res=%d bit, max_ch=%d)",
            s_adc_dev->name,
            CONFIG_AKIRA_WASM_ADC_VREF_MV,
            CONFIG_AKIRA_WASM_ADC_RESOLUTION,
            CONFIG_AKIRA_WASM_ADC_MAX_CHANNELS);

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
        LOG_ERR("ADC channel %d setup failed: %d", ch, ret);
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
    if (akira_adc_init() != 0) {
        return -ENODEV;
    }
    if (channel < 0 || channel >= CONFIG_AKIRA_WASM_ADC_MAX_CHANNELS) {
        return -EINVAL;
    }

    int ret = ensure_channel(channel);
    if (ret < 0) {
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
    if (ret < 0) {
        LOG_ERR("ADC read ch=%d err=%d", channel, ret);
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
        LOG_ERR("ADC raw_to_mv ch=%d err=%d", channel, ret);
        return ret;
    }

    LOG_DBG("ADC ch=%d raw=%d mv=%d", channel, raw, mv);
    return (int)mv;
}

/* -------------------------------------------------------------------------- */
/* WASM native exports                                                         */
/* -------------------------------------------------------------------------- */

#ifdef CONFIG_AKIRA_WASM_RUNTIME

int akira_native_adc_read(wasm_exec_env_t exec_env, int32_t channel)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_ADC, -EPERM);
    return akira_adc_read((int)channel);
}

int akira_native_adc_read_mv(wasm_exec_env_t exec_env, int32_t channel)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_ADC, -EPERM);
    return akira_adc_read_mv((int)channel);
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
