/**
 * @file capability.h
 * @brief AkiraOS Capability-Based Permission System
 */

#ifndef AKIRA_CAPABILITY_H
#define AKIRA_CAPABILITY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Capability flags (bitmask)
     */
    typedef enum
    {
        CAP_NONE = 0,

        /* Display capabilities */
        CAP_DISPLAY_READ = (1 << 0),
        CAP_DISPLAY_WRITE = (1 << 1),

        /* Input capabilities */
        CAP_INPUT_READ = (1 << 2),
        CAP_INPUT_CALLBACK = (1 << 3),

        /* RF capabilities */
        CAP_RF_INIT = (1 << 4),
        CAP_RF_TRANSCEIVE = (1 << 5),
        CAP_RF_CONFIG = (1 << 6),

        /* Sensor capabilities */
        CAP_SENSOR_IMU = (1 << 7),
        CAP_SENSOR_ENV = (1 << 8),
        CAP_SENSOR_POWER = (1 << 9),
        CAP_SENSOR_LIGHT = (1 << 10),

        /* Storage capabilities */
        CAP_STORAGE_READ = (1 << 11),
        CAP_STORAGE_WRITE = (1 << 12),

        /* Network capabilities */
        CAP_NETWORK_HTTP = (1 << 13),
        CAP_NETWORK_MQTT = (1 << 14),
        CAP_NETWORK_RAW = (1 << 15),

        /* System capabilities */
        CAP_SYSTEM_INFO = (1 << 16),
        CAP_SYSTEM_REBOOT = (1 << 17),
        CAP_SYSTEM_SETTINGS = (1 << 18),

        /* Bluetooth capabilities */
        CAP_BT_ADVERTISE = (1 << 19),
        CAP_BT_CONNECT = (1 << 20),
        CAP_BT_HID = (1 << 21),

        /* IPC capabilities */
        CAP_IPC_SEND = (1 << 22),
        CAP_IPC_RECEIVE = (1 << 23),
        CAP_IPC_SHM = (1 << 24),
    } akira_capability_t;

    /**
     * @brief Capability set for a container
     */
    typedef struct
    {
        uint32_t flags;
        char container_name[32];
    } akira_cap_set_t;

    /**
     * @brief Initialize capability system
     * @return 0 on success
     */
    int capability_init(void);

    /**
     * @brief Set capabilities for a container
     * @param name Container name
     * @param caps Capability bitmask
     * @return 0 on success
     */
    int capability_set(const char *name, uint32_t caps);

    /**
     * @brief Check if container has capability
     * @param name Container name
     * @param cap Capability to check
     * @return true if capability is granted
     */
    bool capability_check(const char *name, akira_capability_t cap);

    /**
     * @brief Get all capabilities for container
     * @param name Container name
     * @return Capability bitmask
     */
    uint32_t capability_get(const char *name);

    /**
     * @brief Revoke capability from container
     * @param name Container name
     * @param cap Capability to revoke
     * @return 0 on success
     */
    int capability_revoke(const char *name, akira_capability_t cap);

    /**
     * @brief Parse capability string from manifest
     * @param cap_str Capability string (e.g., "display.write")
     * @return Capability flag, or CAP_NONE if invalid
     */
    akira_capability_t capability_from_string(const char *cap_str);

    /**
     * @brief Get capability name string
     * @param cap Capability flag
     * @return Capability name string
     */
    const char *capability_to_string(akira_capability_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CAPABILITY_H */
