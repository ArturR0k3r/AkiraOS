/**
 * @file power_manager.c
 * @brief AkiraOS Power Management Implementation
 */

#include "power_manager.h"
#include "../drivers/ina219.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/pm.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_power_manager, LOG_LEVEL_INF);

// TODO: Integrate with ESP32 deep sleep APIs
// TODO: Add ULP coprocessor support
// TODO: Add power consumption tracking
// TODO: Add thermal management

#define MAX_CONTAINERS 16

static struct
{
    bool initialized;
    akira_power_mode_t current_mode;
    uint32_t wake_sources;
    struct
    {
        char name[32];
        akira_power_policy_t policy;
    } container_policies[MAX_CONTAINERS];
    int policy_count;
} g_pm_state = {0};

int power_manager_init(void)
{
    memset(&g_pm_state, 0, sizeof(g_pm_state));
    g_pm_state.current_mode = POWER_MODE_ACTIVE;
    g_pm_state.initialized = true;

    // TODO: Initialize battery monitoring
    // TODO: Configure default wake sources
    // TODO: Read last power mode from NVS

    LOG_INF("Power manager initialized");
    return 0;
}

int akira_pm_set_mode(akira_power_mode_t mode)
{
    // TODO: Implement actual power mode transitions

    if (!g_pm_state.initialized)
    {
        return -1;
    }

    LOG_INF("Power mode: %d -> %d", g_pm_state.current_mode, mode);

    switch (mode)
    {
    case POWER_MODE_ACTIVE:
        // TODO: Full speed, all peripherals on
        break;

    case POWER_MODE_IDLE:
        // TODO: Clock gate CPU when idle
        // pm_state_force(PM_STATE_RUNTIME_IDLE);
        break;

    case POWER_MODE_LIGHT_SLEEP:
        // TODO: Configure wake sources
        // TODO: Enter light sleep
        // esp_light_sleep_start();
        LOG_WRN("Light sleep not implemented");
        break;

    case POWER_MODE_DEEP_SLEEP:
        // TODO: Save state to RTC memory
        // TODO: Configure wake sources
        // TODO: Enter deep sleep
        // esp_deep_sleep_start();
        LOG_WRN("Deep sleep not implemented");
        break;

    case POWER_MODE_HIBERNATE:
        // TODO: Only RTC timer wake
        // esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        // esp_sleep_enable_timer_wakeup(...);
        // esp_deep_sleep_start();
        LOG_WRN("Hibernate not implemented");
        break;

    default:
        LOG_ERR("Invalid power mode: %d", mode);
        return -2;
    }

    g_pm_state.current_mode = mode;
    return 0;
}

akira_power_mode_t akira_pm_get_mode(void)
{
    return g_pm_state.current_mode;
}

int akira_pm_wake_on_gpio(uint32_t pin, int edge)
{
    // TODO: Configure GPIO wake source
    // esp_sleep_enable_ext0_wakeup(pin, level);
    // esp_sleep_enable_ext1_wakeup(mask, mode);

    LOG_INF("Configure GPIO wake: pin=%u, edge=%d", pin, edge);

    g_pm_state.wake_sources |= WAKE_SOURCE_GPIO;

    return -3; // Not implemented
}

int akira_pm_wake_on_timer(uint32_t ms)
{
    // TODO: Configure RTC timer wake
    // esp_sleep_enable_timer_wakeup(ms * 1000ULL);

    LOG_INF("Configure timer wake: %u ms", ms);

    g_pm_state.wake_sources |= WAKE_SOURCE_TIMER;

    return -3; // Not implemented
}

int akira_pm_get_battery_level(uint8_t *percent)
{
    if (!percent)
    {
        return -1;
    }

    // TODO: Read from INA219 or ADC
    // TODO: Apply voltage-to-percent curve

    *percent = 75; // Placeholder

    return 0;
}

int akira_pm_get_battery_status(akira_battery_status_t *status)
{
    if (!status)
    {
        return -1;
    }

    // TODO: Read from INA219
    // TODO: Detect charging state
    // TODO: Calculate remaining time

    status->level_percent = 75;
    status->voltage = 3.7f;
    status->current = 0.15f;
    status->charging = false;
    status->low_battery = false;

    return 0;
}

int akira_pm_enable_low_power_mode(bool enable)
{
    // TODO: Enable/disable automatic power management
    // TODO: Adjust peripheral clock speeds
    // TODO: Enable DFS (dynamic frequency scaling)

    LOG_INF("Low power mode: %s", enable ? "enabled" : "disabled");

    return 0;
}

int akira_pm_set_policy(const char *name, akira_power_policy_t policy)
{
    if (!name)
    {
        return -1;
    }

    // Find existing or add new
    for (int i = 0; i < g_pm_state.policy_count; i++)
    {
        if (strcmp(g_pm_state.container_policies[i].name, name) == 0)
        {
            g_pm_state.container_policies[i].policy = policy;
            LOG_INF("Updated policy for %s: %d", name, policy);
            return 0;
        }
    }

    if (g_pm_state.policy_count >= MAX_CONTAINERS)
    {
        return -2;
    }

    strncpy(g_pm_state.container_policies[g_pm_state.policy_count].name,
            name, sizeof(g_pm_state.container_policies[0].name) - 1);
    g_pm_state.container_policies[g_pm_state.policy_count].policy = policy;
    g_pm_state.policy_count++;

    LOG_INF("Set policy for %s: %d", name, policy);
    return 0;
}

akira_power_policy_t akira_pm_get_aggregate_policy(void)
{
    // TODO: Use most restrictive policy from all active containers
    // PERFORMANCE > BALANCED > LOW_POWER

    akira_power_policy_t aggregate = POWER_POLICY_LOW_POWER;

    for (int i = 0; i < g_pm_state.policy_count; i++)
    {
        if (g_pm_state.container_policies[i].policy < aggregate)
        {
            aggregate = g_pm_state.container_policies[i].policy;
        }
    }

    return aggregate;
}
