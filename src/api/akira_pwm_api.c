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
 * period = PWM clock / freq_hz (in controller cycles)
 * pulse  = period * duty_pct / 100
 *
 * Requires a "pwm-leds" or similar node with aliases pwm0, pwm1 …
 * in the board overlay, or device nodes accessible by label.
 *
 * Containment.  The LEDC controller also drives system outputs (on
 * AkiraConsole Prod, channel 0 is the display backlight), so a channel is
 * usable only if, all decided from the devicetree:
 *   - it is a channel the controller defines (a channel@N child),
 *   - no okay node's "pwms" property uses it (backlight, LEDs, buzzer ...),
 *   - it does not share an LEDC timer with such a channel (a timer has one
 *     frequency; an app's frequency would break the system channel's next
 *     update),
 *   - its output pad, from the controller's pinctrl, is a pin apps may use
 *     (CONFIG_AKIRA_WASM_GPIO_APP_PINS minus system pins).
 * A channel belongs to the app that set it until it disables it; every
 * channel an app left running is stopped (output held low) when it ends,
 * however it ends.
 */

#include "akira_pwm_api.h"
#include <runtime/security.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <akira_app_containment.h>

#define AKIRA_PWM_MAX_CHANNELS  4
#define AKIRA_PWM_MAX_FREQ_HZ   10000000
/* Fewer cycles per period than this leaves no usable duty resolution, and
 * 0 divides by zero inside the ESP32 LEDC driver while it holds its lock. */
#define AKIRA_PWM_MIN_PERIOD_CYCLES 2U

/*
 * PWM devices by logical channel index.
 * On ESP32-S3, all channels share the single LEDC controller (ledc0).
 * The WASM channel argument maps directly to the LEDC hardware channel (0-7).
 */
static const char *const s_pwm_names[] = { "ledc0", "ledc0", "ledc0", "ledc0" };
BUILD_ASSERT(ARRAY_SIZE(s_pwm_names) >= AKIRA_PWM_MAX_CHANNELS,
             "s_pwm_names must cover AKIRA_PWM_MAX_CHANNELS entries");

static const struct device *s_pwm_devs[AKIRA_PWM_MAX_CHANNELS];

/* -------------------------------------------------------------------------- */
/* Channel policy (compile time, from the devicetree)                          */
/* -------------------------------------------------------------------------- */

#if DT_NODE_HAS_STATUS(DT_NODELABEL(ledc0), okay)
#define PWM_CTLR DT_NODELABEL(ledc0)

/* Channels a "pwms" entry of any okay node points at on this controller. */
#define PWM_PHA_CHAN_BIT(node, prop, idx)                                                 \
    | COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(node, prop, idx, channel),                       \
                  ((DT_SAME_NODE(DT_PHANDLE_BY_IDX(node, prop, idx), PWM_CTLR)            \
                        ? BIT(DT_PHA_BY_IDX(node, prop, idx, channel) & 31) : 0U)),      \
                  (0U))
#define PWM_NODE_SYS_CHANS(node) \
    COND_CODE_1(DT_NODE_HAS_PROP(node, pwms), (DT_FOREACH_PROP_ELEM(node, pwms, PWM_PHA_CHAN_BIT)), ())
#define PWM_SYSTEM_CHANNELS (0U DT_FOREACH_STATUS_OKAY_NODE(PWM_NODE_SYS_CHANS))

/* Channels the controller defines (the driver configures every child). */
#define PWM_CHILD_BIT(child) | BIT(DT_REG_ADDR(child) & 31)
#define PWM_DT_CHANNELS (0U DT_FOREACH_CHILD(PWM_CTLR, PWM_CHILD_BIT))

/* LEDC timer of each defined channel, +1 (0 = channel not defined). */
#define PWM_CHILD_TIMER(child) [DT_REG_ADDR(child) & 15] = DT_PROP(child, timer) + 1,
static const uint8_t s_pwm_timer[16] = { DT_FOREACH_CHILD(PWM_CTLR, PWM_CHILD_TIMER) };

#if defined(CONFIG_PWM_LED_ESP32) && defined(CONFIG_GPIO)
#include "akira_gpio_api.h"
#include <zephyr/dt-bindings/pinctrl/esp-pinctrl-common.h>
#include <soc/soc_caps.h>
/* LEDC_LS_SIG_OUT0_IDX, the value the HAL's ledc_periph_signal table holds:
 * that table lives in ledc_periph.c, which Zephyr's LEDC driver doesn't link. */
#include <soc/gpio_sig_map.h>

/* Every pinmux cell of the controller's default pinctrl state. */
#define PWM_PINMUX_CELL(node, prop, idx) DT_PROP_BY_IDX(node, prop, idx),
#define PWM_PINCTRL_GROUP(group) \
    COND_CODE_1(DT_NODE_HAS_PROP(group, pinmux),                                          \
                (DT_FOREACH_PROP_ELEM_SEP(group, pinmux, PWM_PINMUX_CELL, ())), ())
#define PWM_PINCTRL_STATE(node, prop, idx) \
    DT_FOREACH_CHILD(DT_PHANDLE_BY_IDX(node, prop, idx), PWM_PINCTRL_GROUP)
static const uint32_t s_ledc_pinmux[] = {
    COND_CODE_1(DT_NODE_HAS_PROP(PWM_CTLR, pinctrl_0),
                (DT_FOREACH_PROP_ELEM(PWM_CTLR, pinctrl_0, PWM_PINCTRL_STATE)), ())
    UINT32_MAX /* end marker, keeps the array non-empty */
};

BUILD_ASSERT(AKIRA_PWM_MAX_CHANNELS <= SOC_LEDC_CHANNEL_NUM,
             "API channels must all be low-speed LEDC channels");

/* Every pad LEDC channel @p ch is routed to must be one apps may drive. A
 * channel routed nowhere drives nothing and is allowed. */
static bool pwm_channel_pads_ok(uint32_t ch)
{
    /* Same mapping as the driver: channels below SOC_LEDC_CHANNEL_NUM are
     * low-speed, output signal = sig_out0_idx + channel. */
    uint32_t sig = LEDC_LS_SIG_OUT0_IDX + ch;

    for (size_t i = 0; i + 1 < ARRAY_SIZE(s_ledc_pinmux); i++) {
        uint32_t cell = s_ledc_pinmux[i];

        if (((cell >> ESP32_PIN_SIGO_SHIFT) & ESP32_PIN_SIGO_MASK) == sig &&
            !akira_gpio_pin_pwm_pad_allowed((cell >> ESP32_PIN_NUM_SHIFT) &
                                            ESP32_PIN_NUM_MASK)) {
            return false;
        }
    }
    return true;
}
#else
/* No pad decoding for this controller: the channel rules above still hold. */
static bool pwm_channel_pads_ok(uint32_t ch)
{
    ARG_UNUSED(ch);
    return true;
}
#endif

/* 0, or why an app may not use @p channel (already range-checked). */
static int pwm_channel_policy(int32_t channel)
{
    uint32_t ch = (uint32_t)channel;

    if ((PWM_DT_CHANNELS & BIT(ch)) == 0U) {
        return -ENODEV;
    }
    if ((PWM_SYSTEM_CHANNELS & BIT(ch)) != 0U) {
        return -EACCES;
    }
    for (uint32_t s = 0; s < ARRAY_SIZE(s_pwm_timer); s++) {
        if ((PWM_SYSTEM_CHANNELS & BIT(s)) != 0U && s_pwm_timer[s] != 0U &&
            s_pwm_timer[s] == s_pwm_timer[ch]) {
            return -EACCES;
        }
    }
    if (!pwm_channel_pads_ok(ch)) {
        return -EACCES;
    }
    return 0;
}
#else /* no LEDC controller: nothing the API can drive */
static int pwm_channel_policy(int32_t channel)
{
    ARG_UNUSED(channel);
    return -ENODEV;
}
#endif

/* -------------------------------------------------------------------------- */
/* Ownership                                                                   */
/* -------------------------------------------------------------------------- */

/* Owning slot + 1 per channel (0 = free). Tiny, so plain .bss. */
static uint8_t s_pwm_owner[AKIRA_PWM_MAX_CHANNELS];
static struct k_spinlock s_pwm_lock;

/* Claim @p ch for @p slot. -EBUSY if another app has it. *fresh says
 * whether this call took it (so a failed set can give it back). */
static int pwm_claim(int32_t ch, int slot, bool *fresh)
{
    int ret = 0;
    k_spinlock_key_t key = k_spin_lock(&s_pwm_lock);

    *fresh = false;
    if (s_pwm_owner[ch] == 0U) {
        s_pwm_owner[ch] = (uint8_t)(slot + 1);
        *fresh = true;
    } else if (s_pwm_owner[ch] != (uint8_t)(slot + 1)) {
        ret = -EBUSY;
    }
    k_spin_unlock(&s_pwm_lock, key);
    return ret;
}

static void pwm_unclaim(int32_t ch)
{
    k_spinlock_key_t key = k_spin_lock(&s_pwm_lock);

    s_pwm_owner[ch] = 0U;
    k_spin_unlock(&s_pwm_lock, key);
}

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
    const struct device *dev = device_get_by_dt_nodelabel(s_pwm_names[channel]);
    if (!dev || !device_is_ready(dev)) {
        LOG_DBG("pwm: device %s not ready", s_pwm_names[channel]);
        return NULL;
    }
    s_pwm_devs[channel] = dev;
    LOG_INF("pwm: device %s resolved", s_pwm_names[channel]);
    return dev;
}

/* Hold the output low (duty 0 stops the channel). Takes the driver's lock. */
static int pwm_stop_output(const struct device *dev, int32_t channel)
{
    return pwm_set_cycles(dev, (uint32_t)channel, 1000U, 0U, 0);
}

/* Release hook: stop every channel the app left running and free it. The
 * driver call is short; the app thread never dies holding the driver's lock
 * because it only calls it inside a critical section. */
static void pwm_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);

    if (slot < 0) {
        return;
    }
    for (int32_t ch = 0; ch < AKIRA_PWM_MAX_CHANNELS; ch++) {
        if (s_pwm_owner[ch] != (uint8_t)(slot + 1)) {
            continue;
        }
        const struct device *dev = get_pwm_dev(ch);

        if (dev) {
            (void)pwm_stop_output(dev, ch);
        }
        pwm_unclaim(ch);
        LOG_DBG("pwm: stopped channel %d left running by app slot %d", ch, slot);
    }
}

AKIRA_APP_RESOURCE_DEFINE(pwm_channels, 50, NULL, pwm_release);

/* -------------------------------------------------------------------------- */
/* WASM native exports                                                         */
/* -------------------------------------------------------------------------- */

int akira_native_pwm_set(wasm_exec_env_t exec_env,
                          int32_t channel, int32_t freq_hz, int32_t duty_pct)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_PWM, -EPERM);

    if (freq_hz < 1 || freq_hz > AKIRA_PWM_MAX_FREQ_HZ) {
        LOG_DBG("pwm_set: invalid freq_hz=%d", freq_hz);
        return -EINVAL;
    }
    if (duty_pct < 0 || duty_pct > 100) {
        LOG_DBG("pwm_set: invalid duty_pct=%d", duty_pct);
        return -EINVAL;
    }
    if (channel < 0 || channel >= AKIRA_PWM_MAX_CHANNELS) {
        return -ENODEV;
    }
    int ret = pwm_channel_policy(channel);
    if (ret < 0) {
        return ret;
    }
    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0 || slot >= UINT8_MAX) {
        return -EPERM;
    }
    const struct device *dev = get_pwm_dev(channel);
    if (!dev) {
        return -ENODEV;
    }
    bool fresh;
    ret = pwm_claim(channel, slot, &fresh);
    if (ret < 0) {
        return ret;
    }

    /* The driver takes its command lock (forever) inside: a stop must not
     * abort the app while it holds it. */
    akira_native_critical_enter(exec_env);
    uint64_t cps = 0;
    ret = pwm_get_cycles_per_sec(dev, (uint32_t)channel, &cps);
    if (ret == 0) {
        /* Cycles, not ns: pwm_set() would round a short period at a slow
         * channel clock down to 0 cycles. */
        uint64_t period = cps / (uint64_t)freq_hz;

        if (period < AKIRA_PWM_MIN_PERIOD_CYCLES || period > UINT32_MAX) {
            ret = -EINVAL;
        } else {
            uint32_t pulse = (uint32_t)(period * (uint32_t)duty_pct / 100U);

            ret = pwm_set_cycles(dev, (uint32_t)channel, (uint32_t)period, pulse, 0);
        }
    }
    akira_native_critical_exit(exec_env);

    if (ret < 0) {
        if (fresh) {
            pwm_unclaim(channel);
        }
        LOG_DBG("pwm_set: channel=%d freq=%d duty=%d err=%d",
                channel, freq_hz, duty_pct, ret);
    } else {
        LOG_DBG("pwm_set: channel=%d freq=%dHz duty=%d%%", channel, freq_hz, duty_pct);
    }
    return ret;
}

int akira_native_pwm_disable(wasm_exec_env_t exec_env, int32_t channel)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_PWM, -EPERM);

    if (channel < 0 || channel >= AKIRA_PWM_MAX_CHANNELS) {
        return -ENODEV;
    }
    int ret = pwm_channel_policy(channel);
    if (ret < 0) {
        return ret;
    }
    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0 || slot >= UINT8_MAX) {
        return -EPERM;
    }
    const struct device *dev = get_pwm_dev(channel);
    if (!dev) {
        return -ENODEV;
    }
    /* Another app's channel is not ours to stop. Claiming a free one first
     * keeps another app from taking it while we stop it. */
    bool fresh;
    if (pwm_claim(channel, slot, &fresh) < 0) {
        return -EPERM;
    }

    akira_native_critical_enter(exec_env);
    ret = pwm_stop_output(dev, channel);
    akira_native_critical_exit(exec_env);

    if (ret < 0) {
        LOG_DBG("pwm_disable: channel=%d err=%d", channel, ret);
    } else {
        LOG_DBG("pwm_disable: channel=%d", channel);
    }
    /* Stopped: free for any app again. A failed stop keeps a channel the
     * app already owned (it may still be running) for the release hook. */
    if (ret == 0 || fresh) {
        pwm_unclaim(channel);
    }
    return ret;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_PWM))
#include <akira_native_registry.h>

static const NativeSymbol akira_pwm_natives[] = {
    {"pwm_set", (void *)akira_native_pwm_set, "(iii)i", NULL},
    {"pwm_disable", (void *)akira_native_pwm_disable, "(i)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_pwm_api, "env", akira_pwm_natives);
#endif
