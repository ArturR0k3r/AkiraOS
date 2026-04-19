/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_pwm
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_pwm, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_pwm_api.c
 * @brief PWM channel control API for WASM applications
 *
 * Maps logical channel numbers to Zephyr PWM devices.
 * period_ns = 1,000,000,000 / freq_hz
 * pulse_ns  = period_ns * duty_pct / 100
 *
 * Requires a "pwm-leds" or similar node with aliases pwm0, pwm1 …
 * in the board overlay, or device nodes accessible by label.
 */

#include "akira_pwm_api.h"
#include <runtime/security.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <errno.h>
#include <string.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#define AKIRA_PWM_MAX_CHANNELS  4
#define AKIRA_PWM_MAX_FREQ_HZ   10000000
#define AKIRA_PWM_NS_PER_SEC    1000000000ULL

/*
 * PWM devices by logical channel index.
 * Channel 0 → "pwm0", channel 1 → "pwm1" …
 * Most boards expose a single LEDC/PWM controller — use channel 0.
 */
static const char *const s_pwm_names[] = { "pwm0", "pwm1", "pwm2", "pwm3" };
BUILD_ASSERT(ARRAY_SIZE(s_pwm_names) >= AKIRA_PWM_MAX_CHANNELS,
             "s_pwm_names must cover AKIRA_PWM_MAX_CHANNELS entries");

static const struct device *s_pwm_devs[AKIRA_PWM_MAX_CHANNELS];

/* On ESP32 LEDC, each channel is channel 0 of its dedicated PWM device node */
#define AKIRA_PWM_CHANNEL_ID  0

/* -------------------------------------------------------------------------- */
/* Lazy device resolution                                                      */
/* -------------------------------------------------------------------------- */

static const struct device *get_pwm_dev(int32_t channel)
{
    if (channel < 0 || channel >= AKIRA_PWM_MAX_CHANNELS) {
        return NULL;
    }
    if (s_pwm_devs[channel] != NULL) {
        return s_pwm_devs[channel];
    }
    const struct device *dev = device_get_binding(s_pwm_names[channel]);
    if (!dev || !device_is_ready(dev)) {
        LOG_ERR("pwm: device %s not ready", s_pwm_names[channel]);
        return NULL;
    }
    s_pwm_devs[channel] = dev;
    LOG_INF("pwm: device %s resolved", s_pwm_names[channel]);
    return dev;
}

/* -------------------------------------------------------------------------- */
/* WASM native exports                                                         */
/* -------------------------------------------------------------------------- */

int akira_native_pwm_set(wasm_exec_env_t exec_env,
                          int32_t channel, int32_t freq_hz, int32_t duty_pct)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_PWM, -EPERM);

    if (freq_hz < 1 || freq_hz > AKIRA_PWM_MAX_FREQ_HZ) {
        LOG_ERR("pwm_set: invalid freq_hz=%d", freq_hz);
        return -EINVAL;
    }
    if (duty_pct < 0 || duty_pct > 100) {
        LOG_ERR("pwm_set: invalid duty_pct=%d", duty_pct);
        return -EINVAL;
    }

    const struct device *dev = get_pwm_dev(channel);
    if (!dev) {
        return -ENODEV;
    }

    uint32_t period_ns = (uint32_t)(AKIRA_PWM_NS_PER_SEC / (uint64_t)freq_hz);
    uint32_t pulse_ns  = (uint32_t)((uint64_t)period_ns * (uint32_t)duty_pct / 100ULL);

    int ret = pwm_set(dev, AKIRA_PWM_CHANNEL_ID, period_ns, pulse_ns, 0);
    if (ret < 0) {
        LOG_ERR("pwm_set: channel=%d freq=%d duty=%d err=%d",
                channel, freq_hz, duty_pct, ret);
    } else {
        LOG_DBG("pwm_set: channel=%d freq=%dHz duty=%d%% period=%uns pulse=%uns",
                channel, freq_hz, duty_pct, period_ns, pulse_ns);
    }
    return ret;
}

int akira_native_pwm_disable(wasm_exec_env_t exec_env, int32_t channel)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_PWM, -EPERM);

    const struct device *dev = get_pwm_dev(channel);
    if (!dev) {
        return -ENODEV;
    }

    /* Set pulse_ns = 0 to hold output low */
    int ret = pwm_set(dev, AKIRA_PWM_CHANNEL_ID, 1000000 /* 1ms period */, 0, 0);
    if (ret < 0) {
        LOG_ERR("pwm_disable: channel=%d err=%d", channel, ret);
    } else {
        LOG_DBG("pwm_disable: channel=%d", channel);
    }
    return ret;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
