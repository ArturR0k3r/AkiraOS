/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_power_manager
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_power_manager, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file power_manager.c
 * @brief AkiraOS Power Management — sleep modes, battery monitoring, wake sources.
 *
 * Battery readout priority:
 *   1. Zephyr fuel gauge driver (CONFIG_FUEL_GAUGE=y, any DTS-bound gauge IC)
 *   2. INA219 via Zephyr sensor API (CONFIG_INA219=y, DTS node compatible ti,ina219)
 *   3. ADC voltage divider   (CONFIG_AKIRA_BATTERY_ADC=y, DTS alias battery_voltage)
 *   4. -ENODEV  (no hardware present)
 *
 * Deep / light sleep delegate to pm_state_force(); the SoC-specific Zephyr PM
 * backend (e.g. ESP32, nRF, STM32) maps these to correct hardware states.
 */

#include "power_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/state.h>
#include <string.h>
#include <errno.h>

#ifdef CONFIG_AKIRA_MODULE_RF
#include <api/akira_rf_api.h>
#endif
#if defined(CONFIG_BT)
#include <connectivity/bluetooth/bt_manager.h>
#endif

#if defined(CONFIG_SOC_ESP32S3) && !defined(CONFIG_PM)
/* CONFIG_PM=n: deep sleep goes via sys_poweroff() -> esp_deep_sleep_start(). */
#include <zephyr/sys/poweroff.h>
#include <esp_sleep.h>
#endif

#ifdef CONFIG_FUEL_GAUGE
#include <zephyr/drivers/fuel_gauge.h>
#endif

#ifdef CONFIG_INA219
/* Use INA219 via the standard Zephyr sensor API — no custom driver needed.
 * DTS: compatible = "ti,ina219"; the Zephyr upstream driver is selected by
 * CONFIG_INA219 when the node is present in the board overlay. */
#include <zephyr/drivers/sensor.h>
#endif

#ifdef CONFIG_AKIRA_BATTERY_ADC
/* ADC battery voltage via a resistor divider wired to an ADC channel.
 * Board overlay must define a DTS alias: battery-voltage = &adc0;
 * and set CONFIG_AKIRA_BATTERY_ADC_CHANNEL / _VREF_MV / _VDIV_MUL.
 * Vadc_mv * CONFIG_AKIRA_BATTERY_ADC_VDIV_MUL / 1000 = Vbat_mv */
#include <zephyr/drivers/adc.h>
#endif

#define MAX_CONTAINERS 16

/* Voltage-to-percent lookup for a typical single-cell Li-Ion (3.0 V – 4.2 V). */
#if defined(CONFIG_INA219) || defined(CONFIG_AKIRA_BATTERY_ADC)
static const struct {
    float voltage;
    uint8_t percent;
} k_lipo_curve[] = {
    { 4.20f, 100 }, { 4.10f, 95 }, { 4.00f, 90 }, { 3.90f, 80 },
    { 3.80f, 70  }, { 3.70f, 60 }, { 3.60f, 45 }, { 3.50f, 30 },
    { 3.40f, 15  }, { 3.30f,  5 }, { 3.00f,  0 },
};
#endif /* CONFIG_INA219 || CONFIG_AKIRA_BATTERY_ADC */

static struct {
    bool            initialized;
    akira_power_mode_t current_mode;
    uint32_t        wake_sources;
    bool            auto_low_power;
    struct {
        char               name[32];
        akira_power_policy_t policy;
    } container_policies[MAX_CONTAINERS];
    int             policy_count;
#ifdef CONFIG_FUEL_GAUGE
    const struct device *fuel_gauge;
#endif
#ifdef CONFIG_INA219
    const struct device *ina219;    /* Zephyr sensor API handle */
#endif
#ifdef CONFIG_AKIRA_BATTERY_ADC
    const struct device *adc_dev;
    bool  adc_ready;
    struct adc_channel_cfg adc_ch_cfg;
    int16_t adc_buf;
#endif
} g_pm = {0};

/* ---------- helpers ---------- */

#if defined(CONFIG_INA219) || defined(CONFIG_AKIRA_BATTERY_ADC)
static uint8_t voltage_to_percent(float v)
{
    if (v >= k_lipo_curve[0].voltage) {
        return 100;
    }
    for (int i = 0; i < (int)ARRAY_SIZE(k_lipo_curve) - 1; i++) {
        if (v >= k_lipo_curve[i + 1].voltage) {
            float range  = k_lipo_curve[i].voltage - k_lipo_curve[i + 1].voltage;
            float offset = v - k_lipo_curve[i + 1].voltage;
            uint8_t pct_range = k_lipo_curve[i].percent - k_lipo_curve[i + 1].percent;
            return k_lipo_curve[i + 1].percent + (uint8_t)(pct_range * offset / range);
        }
    }
    return 0;
}
#endif /* CONFIG_INA219 || CONFIG_AKIRA_BATTERY_ADC */

/* ---------- init ---------- */

int power_manager_init(void)
{
    memset(&g_pm, 0, sizeof(g_pm));
    g_pm.current_mode  = POWER_MODE_ACTIVE;
    g_pm.auto_low_power = false;

#ifdef CONFIG_FUEL_GAUGE
    /* Bind to the first fuel gauge device on the bus (declared in DTS). */
    /* Prefer the BQ28Z610 fitted on AkiraConsole Production.
     * Falls back to any generic fuel_gauge node if present (other boards). */
#if DT_HAS_COMPAT_STATUS_OKAY(ti_bq28z610)
    g_pm.fuel_gauge = DEVICE_DT_GET_ANY(ti_bq28z610);
#else
    g_pm.fuel_gauge = DEVICE_DT_GET_ANY(zephyr_fuel_gauge);
#endif
    if (!g_pm.fuel_gauge || !device_is_ready(g_pm.fuel_gauge)) {
        LOG_WRN("Fuel gauge device not ready — battery readout unavailable");
        g_pm.fuel_gauge = NULL;
    } else {
        /* Probe SOC immediately — this log is at APPLICATION level and always
         * visible in the terminal, unlike the POST_KERNEL driver init messages. */
        union fuel_gauge_prop_val soc_val = {0};
        int probe_ret = fuel_gauge_get_prop(g_pm.fuel_gauge,
                                            FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE,
                                            &soc_val);
        if (probe_ret == 0) {
            LOG_INF("Fuel gauge: %s — SoC=%u%%",
                    g_pm.fuel_gauge->name,
                    (unsigned)soc_val.relative_state_of_charge);
        } else {
            LOG_WRN("Fuel gauge: %s found but not responding (err=%d) — "
                    "check battery connector J8",
                    g_pm.fuel_gauge->name, probe_ret);
            /* Keep g_pm.fuel_gauge set so periodic retries can recover. */
        }
    }
#endif

#ifdef CONFIG_INA219
    /* Bind to the first ti,ina219 node declared in DTS. */
    g_pm.ina219 = DEVICE_DT_GET_ANY(ti_ina219);
    if (!g_pm.ina219 || !device_is_ready(g_pm.ina219)) {
        LOG_WRN("INA219 device not ready — skipping");
        g_pm.ina219 = NULL;
    } else {
        LOG_INF("INA219 sensor: %s", g_pm.ina219->name);
    }
#endif

#ifdef CONFIG_AKIRA_BATTERY_ADC
    /* Bind to the ADC device aliased as 'battery-voltage' in DTS.
     * Falls back to the first ADC device when the alias is absent. */
#if DT_NODE_HAS_STATUS(DT_ALIAS(battery_voltage), okay)
    g_pm.adc_dev = DEVICE_DT_GET(DT_ALIAS(battery_voltage));
#else
    g_pm.adc_dev = DEVICE_DT_GET_ANY(zephyr_adc);
#endif
    if (g_pm.adc_dev && device_is_ready(g_pm.adc_dev)) {
        g_pm.adc_ch_cfg = (struct adc_channel_cfg){
            .gain             = ADC_GAIN_1,
            .reference        = ADC_REF_INTERNAL,
            .acquisition_time = ADC_ACQ_TIME_DEFAULT,
            .channel_id       = CONFIG_AKIRA_BATTERY_ADC_CHANNEL,
            .differential     = 0,
        };
        int ret = adc_channel_setup(g_pm.adc_dev, &g_pm.adc_ch_cfg);
        g_pm.adc_ready = (ret == 0);
        if (!g_pm.adc_ready) {
            LOG_WRN("ADC battery channel setup failed: %d", ret);
        } else {
            LOG_INF("ADC battery: ch=%d vref=%d mV divider=%d/1000",
                    CONFIG_AKIRA_BATTERY_ADC_CHANNEL,
                    CONFIG_AKIRA_BATTERY_ADC_VREF_MV,
                    CONFIG_AKIRA_BATTERY_ADC_VDIV_MUL);
        }
    } else {
        LOG_WRN("ADC device not ready — battery ADC unavailable");
    }
#endif

    g_pm.initialized = true;
    LOG_INF("Power manager initialized (mode=ACTIVE)");
    return 0;
}

/* ---------- sleep modes ---------- */

int akira_pm_set_mode(akira_power_mode_t mode)
{
    if (!g_pm.initialized) {
        return -EAGAIN;
    }
    if (mode < POWER_MODE_ACTIVE || mode > POWER_MODE_HIBERNATE) {
        LOG_ERR("Invalid power mode: %d", mode);
        return -EINVAL;
    }

    LOG_INF("Power mode: %d -> %d", g_pm.current_mode, mode);

    switch (mode) {
    case POWER_MODE_ACTIVE:
        /* Nothing to do — Zephyr resumes automatically on wake. */
        break;

    case POWER_MODE_IDLE:
        /* Allow Zephyr PM to gate the CPU when idle. */
#ifdef CONFIG_PM
        pm_state_force(0u, &(struct pm_state_info){PM_STATE_RUNTIME_IDLE, 0, 0});
        break;
#else
        LOG_WRN("PM not enabled (CONFIG_PM=n) — idle mode unsupported");
        return -ENOTSUP;
#endif

    case POWER_MODE_LIGHT_SLEEP:
        /* Suspend-to-idle: RAM retained, fast wake.
         * On ESP32 this maps to light sleep via the SoC PM backend. */
#ifdef CONFIG_PM
        pm_state_force(0u, &(struct pm_state_info){PM_STATE_SUSPEND_TO_IDLE, 0, 0});
        break;
#else
        LOG_WRN("PM not enabled (CONFIG_PM=n) — light sleep unsupported");
        return -ENOTSUP;
#endif

    case POWER_MODE_DEEP_SLEEP:
        /* Standby / deep sleep: only RTC + configured wake sources preserved. */
        if (!IS_ENABLED(CONFIG_AKIRA_POWER_DEEP_SLEEP)) {
            LOG_WRN("Deep sleep disabled (CONFIG_AKIRA_POWER_DEEP_SLEEP=n)");
            return -ENOTSUP;
        }
#ifdef CONFIG_PM
        pm_state_force(0u, &(struct pm_state_info){PM_STATE_STANDBY, 0, 0});
        break;
#elif defined(CONFIG_SOC_ESP32S3)
        /* sys_poweroff() does not return — reboots on wake. */
        esp_sleep_enable_ext0_wakeup(0 /* GPIO_NUM_0 */, 0 /* level LOW */);
        LOG_INF("Deep sleep — wake on GPIO0 LOW (HOME button)");
        sys_poweroff();
        break; /* unreachable */
#else
        LOG_WRN("PM not enabled (CONFIG_PM=n) — deep sleep unsupported");
        return -ENOTSUP;
#endif

    case POWER_MODE_HIBERNATE:
        /* Soft-off: only external reset / RTC alarm wakes the device. */
        if (!IS_ENABLED(CONFIG_AKIRA_POWER_DEEP_SLEEP)) {
            LOG_WRN("Hibernate disabled (CONFIG_AKIRA_POWER_DEEP_SLEEP=n)");
            return -ENOTSUP;
        }
#ifdef CONFIG_PM
        pm_state_force(0u, &(struct pm_state_info){PM_STATE_SOFT_OFF, 0, 0});
        break;
#elif defined(CONFIG_SOC_ESP32S3)
        /* sys_poweroff() does not return — reboots on wake. */
        esp_sleep_enable_ext0_wakeup(0 /* GPIO_NUM_0 */, 0 /* level LOW */);
        LOG_INF("Hibernate — wake on GPIO0 LOW (HOME button)");
        sys_poweroff();
        break; /* unreachable */
#else
        LOG_WRN("PM not enabled (CONFIG_PM=n) — hibernate unsupported");
        return -ENOTSUP;
#endif
    }

    g_pm.current_mode = mode;
    return 0;
}

akira_power_mode_t akira_pm_get_mode(void)
{
    return g_pm.current_mode;
}

/* ---------- wake sources ---------- */

int akira_pm_wake_on_gpio(uint32_t pin, int edge)
{
    if (pin > 63) {
        return -EINVAL;
    }
    /* Generic wake-source registration is SoC-specific; here we record intent
     * so that board-specific code / PM notifiers can act on it. */
    g_pm.wake_sources |= WAKE_SOURCE_GPIO;
    LOG_INF("Wake-on-GPIO registered: pin=%u edge=%d", pin, edge);
    /* The Zephyr GPIO interrupt is expected to be configured by the caller via
     * gpio_pin_interrupt_configure() before requesting deep sleep. */
    return 0;
}

int akira_pm_wake_on_timer(uint32_t ms)
{
    if (ms == 0) {
        return -EINVAL;
    }
    g_pm.wake_sources |= WAKE_SOURCE_TIMER;
    /* On Zephyr the PM core enables the RTC/timer wake automatically when
     * pm_state_force() is called; we just track the intent here. */
    LOG_INF("Wake-on-timer: %u ms requested", ms);
    return 0;
}

/* ---------- battery readout ---------- */

int akira_pm_get_battery_level(uint8_t *percent)
{
    if (!percent) {
        return -EINVAL;
    }

#ifdef CONFIG_FUEL_GAUGE
    if (g_pm.fuel_gauge) {
        union fuel_gauge_prop_val val;
        int ret = fuel_gauge_get_prop(g_pm.fuel_gauge,
                                      FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE, &val);
        if (ret == 0) {
            *percent = (uint8_t)val.relative_state_of_charge;
            return 0;
        }
        LOG_DBG("Fuel gauge SoC read failed: %d", ret);
    }
#endif

#ifdef CONFIG_INA219
    if (g_pm.ina219) {
        int ret = sensor_sample_fetch(g_pm.ina219);
        if (ret == 0) {
            struct sensor_value v;
            sensor_channel_get(g_pm.ina219, SENSOR_CHAN_VOLTAGE, &v);
            *percent = voltage_to_percent((float)sensor_value_to_double(&v));
            return 0;
        }
        LOG_WRN("INA219 sample fetch failed: %d", ret);
    }
#endif

#ifdef CONFIG_AKIRA_BATTERY_ADC
    if (g_pm.adc_ready) {
        struct adc_sequence seq = {
            .channels    = BIT(CONFIG_AKIRA_BATTERY_ADC_CHANNEL),
            .buffer      = &g_pm.adc_buf,
            .buffer_size = sizeof(g_pm.adc_buf),
            .resolution  = 12,
        };
        int ret = adc_read(g_pm.adc_dev, &seq);
        if (ret == 0) {
            int32_t mv = g_pm.adc_buf;
            adc_raw_to_millivolts(CONFIG_AKIRA_BATTERY_ADC_VREF_MV,
                                  ADC_GAIN_1, 12, &mv);
            /* Apply voltage-divider scaling: Vbat = Vadc * mul / 1000 */
            int32_t vbat_mv = (int32_t)mv * CONFIG_AKIRA_BATTERY_ADC_VDIV_MUL / 1000;
            *percent = voltage_to_percent((float)vbat_mv / 1000.0f);
            return 0;
        }
        LOG_WRN("ADC battery read failed: %d", ret);
    }
#endif

    /* No hardware available. */
    *percent = 0;
    return -ENODEV;
}

int akira_pm_get_battery_status(akira_battery_status_t *status)
{
    if (!status) {
        return -EINVAL;
    }

    memset(status, 0, sizeof(*status));

#ifdef CONFIG_FUEL_GAUGE
    if (g_pm.fuel_gauge) {
        union fuel_gauge_prop_val soc, volt, curr;
        bool ok = true;

        ok &= (fuel_gauge_get_prop(g_pm.fuel_gauge,
                FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE, &soc) == 0);
        ok &= (fuel_gauge_get_prop(g_pm.fuel_gauge,
                FUEL_GAUGE_VOLTAGE, &volt) == 0);
        ok &= (fuel_gauge_get_prop(g_pm.fuel_gauge,
                FUEL_GAUGE_CURRENT, &curr) == 0);

        if (ok) {
            status->level_percent = (uint8_t)soc.relative_state_of_charge;
            /* Fuel-gauge returns µV and µA — convert to mV and mA. */
            status->voltage_mv    = (int32_t)(volt.voltage / 1000);
            status->current_ma    = (int32_t)(curr.current / 1000);
            /* Positive current = discharge, negative = charging.
             * BQ28Z610 reports charge current as negative. */
            status->charging      = (status->current_ma < 0);
            status->low_battery   = (status->level_percent < CONFIG_AKIRA_BATTERY_LOW_THRESHOLD);
            return 0;
        }
        LOG_WRN("Fuel gauge full-status read incomplete");
    }
#endif

#ifdef CONFIG_INA219
    if (g_pm.ina219) {
        int ret = sensor_sample_fetch(g_pm.ina219);
        if (ret == 0) {
            struct sensor_value volt, curr;
            sensor_channel_get(g_pm.ina219, SENSOR_CHAN_VOLTAGE, &volt);
            sensor_channel_get(g_pm.ina219, SENSOR_CHAN_CURRENT, &curr);
            float v = (float)sensor_value_to_double(&volt);
            float i = (float)sensor_value_to_double(&curr); /* amps */
            status->level_percent = voltage_to_percent(v);
            status->voltage_mv    = (int32_t)(v * 1000.0f);
            status->current_ma    = (int32_t)(i * 1000.0f);
            /* Negative current on shunt = charging (convention depends on
             * shunt orientation; INA219 Zephyr driver reports signed amps). */
            status->charging      = (status->current_ma < -10);
            status->low_battery   = (status->level_percent < CONFIG_AKIRA_BATTERY_LOW_THRESHOLD);
            return 0;
        }
        LOG_WRN("INA219 full-status fetch failed: %d", ret);
    }
#endif

#ifdef CONFIG_AKIRA_BATTERY_ADC
    if (g_pm.adc_ready) {
        struct adc_sequence seq = {
            .channels    = BIT(CONFIG_AKIRA_BATTERY_ADC_CHANNEL),
            .buffer      = &g_pm.adc_buf,
            .buffer_size = sizeof(g_pm.adc_buf),
            .resolution  = 12,
        };
        int ret = adc_read(g_pm.adc_dev, &seq);
        if (ret == 0) {
            int32_t mv = g_pm.adc_buf;
            adc_raw_to_millivolts(CONFIG_AKIRA_BATTERY_ADC_VREF_MV,
                                  ADC_GAIN_1, 12, &mv);
            int32_t vbat_mv = (int32_t)mv * CONFIG_AKIRA_BATTERY_ADC_VDIV_MUL / 1000;
            status->level_percent = voltage_to_percent((float)vbat_mv / 1000.0f);
            status->voltage_mv    = vbat_mv;
            status->current_ma    = 0;     /* ADC gives voltage only */
            status->charging      = false; /* indeterminate without current */
            status->low_battery   = (status->level_percent < CONFIG_AKIRA_BATTERY_LOW_THRESHOLD);
            return 0;
        }
        LOG_WRN("ADC battery full-read failed: %d", ret);
    }
#endif

    return -ENODEV;
}

/* ---------- low-power auto-management ---------- */

int akira_pm_enable_low_power_mode(bool enable)
{
    g_pm.auto_low_power = enable;
    /* When enabled the Zephyr PM subsystem (CONFIG_PM=y) will automatically
     * select the deepest allowed idle state; we just unlock it here. */
    LOG_INF("Auto low-power: %s", enable ? "on" : "off");
    return 0;
}

/* ---------- blank-state subsystem idling ---------- */

/* Sole trigger for blank/wake; callers must not call RF/BT idle fns directly. */
void akira_pm_notify_blank(bool entering)
{
    if (entering) {
        akira_pm_enable_low_power_mode(true);
#ifdef CONFIG_AKIRA_MODULE_RF
        (void)akira_rf_sleep_if_idle();
#endif
#if defined(CONFIG_BT)
        (void)bt_manager_conn_params_idle();
#endif
    } else {
        akira_pm_enable_low_power_mode(false);
#ifdef CONFIG_AKIRA_MODULE_RF
        (void)akira_rf_wake();
#endif
#if defined(CONFIG_BT)
        (void)bt_manager_conn_params_active();
#endif
    }
}

/* ---------- per-app policy ---------- */

int akira_pm_set_policy(const char *name, akira_power_policy_t policy)
{
    if (!name) {
        return -EINVAL;
    }
    for (int i = 0; i < g_pm.policy_count; i++) {
        if (strncmp(g_pm.container_policies[i].name, name,
                    sizeof(g_pm.container_policies[0].name)) == 0) {
            g_pm.container_policies[i].policy = policy;
            LOG_INF("Updated policy '%s': %d", name, policy);
            return 0;
        }
    }
    if (g_pm.policy_count >= MAX_CONTAINERS) {
        LOG_ERR("Policy table full");
        return -ENOMEM;
    }
    strncpy(g_pm.container_policies[g_pm.policy_count].name, name,
            sizeof(g_pm.container_policies[0].name) - 1);
    g_pm.container_policies[g_pm.policy_count].policy = policy;
    g_pm.policy_count++;
    LOG_INF("Set policy '%s': %d", name, policy);
    return 0;
}

akira_power_policy_t akira_pm_get_aggregate_policy(void)
{
    /* Enum order != demand order (DEFAULT=0 < PERFORMANCE=1), rank explicitly. */
    static const uint8_t demand_rank[] = {
        [POWER_POLICY_DEFAULT]     = 1,
        [POWER_POLICY_PERFORMANCE] = 3,
        [POWER_POLICY_BALANCED]    = 2,
        [POWER_POLICY_LOW_POWER]   = 0,
    };
    akira_power_policy_t agg = POWER_POLICY_LOW_POWER;
    for (int i = 0; i < g_pm.policy_count; i++) {
        if (demand_rank[g_pm.container_policies[i].policy] > demand_rank[agg]) {
            agg = g_pm.container_policies[i].policy;
        }
    }
    return agg;
}

/* ---------- insomnia (ref-counted keep-awake) ---------- */

#define AKIRA_PM_INSOMNIA_SLOTS 8
#define AKIRA_PM_INSOMNIA_REASON_LEN 32

/* Handle = (generation << 8) | slot; generation bump on enter() invalidates
 * stale handles from a freed+reused slot (ABA-safe). */
#define AKIRA_PM_INSOMNIA_SLOT_BITS 8
#define AKIRA_PM_INSOMNIA_SLOT_MASK ((1 << AKIRA_PM_INSOMNIA_SLOT_BITS) - 1)

static struct {
    char        reason[AKIRA_PM_INSOMNIA_REASON_LEN];
    int64_t     acquired_ms;
    int64_t     deadline_ms; /* 0 = never expires */
    uint32_t    hold_ms;     /* for renew() to re-arm deadline_ms */
    uint32_t    generation;
    bool        active;
} g_insomnia[AKIRA_PM_INSOMNIA_SLOTS];

static struct k_spinlock g_insomnia_lock; /* not a mutex: ISR/workqueue-safe */

/* Slot index from handle, or -EINVAL if stale/out of range. */
static int insomnia_slot_from_handle(int handle)
{
    if (handle < 0) {
        return -EINVAL;
    }
    int slot = handle & AKIRA_PM_INSOMNIA_SLOT_MASK;
    uint32_t gen = (uint32_t)handle >> AKIRA_PM_INSOMNIA_SLOT_BITS;
    if (slot >= AKIRA_PM_INSOMNIA_SLOTS || !g_insomnia[slot].active ||
        g_insomnia[slot].generation != gen) {
        return -EINVAL;
    }
    return slot;
}

int akira_pm_insomnia_enter(const char *reason, uint32_t max_hold_ms)
{
    int64_t now = k_uptime_get();
    int handle = -ENOMEM;

    k_spinlock_key_t key = k_spin_lock(&g_insomnia_lock);
    for (int i = 0; i < AKIRA_PM_INSOMNIA_SLOTS; i++) {
        if (!g_insomnia[i].active) {
            g_insomnia[i].active      = true;
            g_insomnia[i].generation++; /* invalidates any old handle for this slot */
            strncpy(g_insomnia[i].reason, reason ? reason : "?",
                    sizeof(g_insomnia[i].reason) - 1);
            g_insomnia[i].reason[sizeof(g_insomnia[i].reason) - 1] = '\0';
            g_insomnia[i].acquired_ms = now;
            g_insomnia[i].hold_ms     = max_hold_ms;
            g_insomnia[i].deadline_ms = (max_hold_ms == 0) ? 0 : now + max_hold_ms;
            handle = (int)((g_insomnia[i].generation << AKIRA_PM_INSOMNIA_SLOT_BITS) | (uint32_t)i);
            break;
        }
    }
    k_spin_unlock(&g_insomnia_lock, key);

    if (handle < 0) {
        LOG_WRN("Insomnia table full — '%s' rejected", reason ? reason : "?");
    }
    return handle;
}

int akira_pm_insomnia_exit(int handle)
{
    int ret = 0;
    k_spinlock_key_t key = k_spin_lock(&g_insomnia_lock);
    int slot = insomnia_slot_from_handle(handle);
    if (slot >= 0) {
        g_insomnia[slot].active = false;
    } else {
        ret = -EINVAL;
    }
    k_spin_unlock(&g_insomnia_lock, key);
    return ret;
}

int akira_pm_insomnia_renew(int handle)
{
    int ret = 0;
    k_spinlock_key_t key = k_spin_lock(&g_insomnia_lock);
    int slot = insomnia_slot_from_handle(handle);
    if (slot >= 0) {
        if (g_insomnia[slot].hold_ms != 0) {
            g_insomnia[slot].deadline_ms = k_uptime_get() + g_insomnia[slot].hold_ms;
        }
    } else {
        ret = -EINVAL;
    }
    k_spin_unlock(&g_insomnia_lock, key);
    return ret;
}

int akira_pm_insomnia_count(void)
{
    /* Collect expired holders under the lock, log them after unlock. */
    struct {
        const char *reason;
        int64_t     elapsed;
    } expired[AKIRA_PM_INSOMNIA_SLOTS];
    int n_expired = 0;
    int live = 0;
    int64_t now = k_uptime_get();

    k_spinlock_key_t key = k_spin_lock(&g_insomnia_lock);
    for (int i = 0; i < AKIRA_PM_INSOMNIA_SLOTS; i++) {
        if (!g_insomnia[i].active) {
            continue;
        }
        if (g_insomnia[i].deadline_ms != 0 && now >= g_insomnia[i].deadline_ms) {
            expired[n_expired].reason  = g_insomnia[i].reason;
            expired[n_expired].elapsed = now - g_insomnia[i].acquired_ms;
            n_expired++;
            g_insomnia[i].active = false;
        } else {
            live++;
        }
    }
    k_spin_unlock(&g_insomnia_lock, key);

    for (int i = 0; i < n_expired; i++) {
        LOG_WRN("Insomnia lock expired: '%s' held %lld ms — reaped",
                expired[i].reason ? expired[i].reason : "?",
                (long long)expired[i].elapsed);
    }
    return live;
}

int akira_pm_insomnia_dump(void)
{
    struct {
        const char *reason;
        int64_t     age;
    } live[AKIRA_PM_INSOMNIA_SLOTS];
    int n = 0;
    int64_t now = k_uptime_get();

    k_spinlock_key_t key = k_spin_lock(&g_insomnia_lock);
    for (int i = 0; i < AKIRA_PM_INSOMNIA_SLOTS; i++) {
        if (g_insomnia[i].active) {
            live[n].reason = g_insomnia[i].reason;
            live[n].age    = now - g_insomnia[i].acquired_ms;
            n++;
        }
    }
    k_spin_unlock(&g_insomnia_lock, key);

    LOG_INF("Insomnia holders: %d", n);
    for (int i = 0; i < n; i++) {
        LOG_INF("  '%s' age %lld ms",
                live[i].reason ? live[i].reason : "?", (long long)live[i].age);
    }
    return n;
}

/* Auto-init at APPLICATION level so the fuel gauge I2C device is ready. */
static int power_manager_sys_init(void)
{
    return power_manager_init();
}
SYS_INIT(power_manager_sys_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
