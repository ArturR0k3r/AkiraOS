/**
 * @file power_manager.h
 * @brief AkiraOS Power Management
 */

#ifndef AKIRA_POWER_MANAGER_H
#define AKIRA_POWER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Power modes
     */
    typedef enum
    {
        POWER_MODE_ACTIVE = 0,  // Full speed
        POWER_MODE_IDLE,        // CPU clock gated, RAM on
        POWER_MODE_LIGHT_SLEEP, // CPU off, RAM on, peripherals selectable
        POWER_MODE_DEEP_SLEEP,  // CPU off, RTC RAM only, peripherals off
        POWER_MODE_HIBERNATE    // Everything off except RTC timer
    } akira_power_mode_t;

    /**
     * @brief Wake sources
     */
    typedef enum
    {
        WAKE_SOURCE_NONE = 0,
        WAKE_SOURCE_GPIO = (1 << 0),
        WAKE_SOURCE_TIMER = (1 << 1),
        WAKE_SOURCE_UART = (1 << 2),
        WAKE_SOURCE_BT = (1 << 3),
        WAKE_SOURCE_WIFI = (1 << 4),
        WAKE_SOURCE_ULP = (1 << 5)
    } akira_wake_source_t;

    /**
     * @brief Power policy for apps
     */
    typedef enum
    {
        POWER_POLICY_DEFAULT = 0,
        POWER_POLICY_PERFORMANCE, // Keep CPU active
        POWER_POLICY_BALANCED,    // Allow idle sleep
        POWER_POLICY_LOW_POWER    // Aggressive power saving
    } akira_power_policy_t;

    /**
     * @brief Battery status
     */
    typedef struct
    {
        uint8_t level_percent;
        float voltage;
        float current;
        bool charging;
        bool low_battery;
    } akira_battery_status_t;

    /**
     * @brief Initialize power manager
     * @return 0 on success
     */
    int power_manager_init(void);

    /**
     * @brief Set power mode
     * @param mode Power mode to enter
     * @return 0 on success
     */
    int akira_pm_set_mode(akira_power_mode_t mode);

    /**
     * @brief Get current power mode
     * @return Current power mode
     */
    akira_power_mode_t akira_pm_get_mode(void);

    /**
     * @brief Configure GPIO wake source
     * @param pin GPIO pin number
     * @param edge Edge type (0=low, 1=high, 2=any)
     * @return 0 on success
     */
    int akira_pm_wake_on_gpio(uint32_t pin, int edge);

    /**
     * @brief Configure timer wake source
     * @param ms Wake time in milliseconds
     * @return 0 on success
     */
    int akira_pm_wake_on_timer(uint32_t ms);

    /**
     * @brief Get battery level
     * @param percent Output for battery percentage
     * @return 0 on success
     */
    int akira_pm_get_battery_level(uint8_t *percent);

    /**
     * @brief Get full battery status
     * @param status Output for battery status
     * @return 0 on success
     */
    int akira_pm_get_battery_status(akira_battery_status_t *status);

    /**
     * @brief Enable/disable low power mode
     * @param enable true to enable
     * @return 0 on success
     */
    int akira_pm_enable_low_power_mode(bool enable);

    /**
     * @brief Set power policy for container
     * @param name Container name
     * @param policy Power policy
     * @return 0 on success
     */
    int akira_pm_set_policy(const char *name, akira_power_policy_t policy);

    /**
     * @brief Get aggregated power policy
     * @return System power policy based on all containers
     */
    akira_power_policy_t akira_pm_get_aggregate_policy(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_POWER_MANAGER_H */
