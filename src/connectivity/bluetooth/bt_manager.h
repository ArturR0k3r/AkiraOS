/**
 * @file bt_manager.h
 * @brief Bluetooth Manager for AkiraOS
 *
 * Manages Bluetooth stack initialization, advertising, connections,
 * and coordinates BLE services (HID, OTA, Shell).
 */

#ifndef AKIRA_BT_MANAGER_H
#define AKIRA_BT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*===========================================================================*/
    /* Configuration                                                             */
    /*===========================================================================*/

    /** Bluetooth connection states */
    typedef enum
    {
        BT_STATE_OFF = 0,
        BT_STATE_INITIALIZING,
        BT_STATE_READY,
        BT_STATE_ADVERTISING,
        BT_STATE_CONNECTED,
        BT_STATE_PAIRING,
        BT_STATE_ERROR
    } bt_state_t;

    /** Bluetooth services */
    typedef enum
    {
        BT_SERVICE_HID = 0x01,
        BT_SERVICE_OTA = 0x02,
        BT_SERVICE_SHELL = 0x04,
        BT_SERVICE_ALL = 0x07
    } bt_service_t;

    /** Bluetooth configuration */
    typedef struct
    {
        const char *device_name;
        uint16_t vendor_id;
        uint16_t product_id;
        bt_service_t services;
        bool auto_advertise;
        bool pairable;
    } bt_config_t;

    /** Bluetooth statistics */
    typedef struct
    {
        bt_state_t state;
        uint32_t connections;
        uint32_t disconnections;
        uint32_t bytes_rx;
        uint32_t bytes_tx;
        int8_t rssi;
        bool bonded;
    } bt_stats_t;

    /*===========================================================================*/
    /* Event Callbacks                                                           */
    /*===========================================================================*/

    typedef enum
    {
        BT_EVENT_READY,
        BT_EVENT_CONNECTED,
        BT_EVENT_DISCONNECTED,
        BT_EVENT_PAIRING_REQUEST,
        BT_EVENT_PAIRED,
        BT_EVENT_UNPAIRED,
        BT_EVENT_ERROR
    } bt_event_t;

    typedef void (*bt_event_callback_t)(bt_event_t event, void *data, void *user_data);

    /*===========================================================================*/
    /* Bluetooth Manager API                                                     */
    /*===========================================================================*/

    /**
     * @brief Initialize Bluetooth manager
     * @param config Configuration options (NULL for defaults)
     * @return 0 on success
     */
    int bt_manager_init(const bt_config_t *config);

    /**
     * @brief Deinitialize Bluetooth manager
     * @return 0 on success
     */
    int bt_manager_deinit(void);

    /**
     * @brief Start Bluetooth advertising
     * @return 0 on success
     */
    int bt_manager_start_advertising(void);

    /**
     * @brief Stop Bluetooth advertising
     * @return 0 on success
     */
    int bt_manager_stop_advertising(void);

    /**
     * @brief Disconnect current connection
     * @return 0 on success
     */
    int bt_manager_disconnect(void);

    /**
     * @brief Get current Bluetooth state
     * @return Current state
     */
    bt_state_t bt_manager_get_state(void);

    /**
     * @brief Get Bluetooth statistics
     * @param stats Output buffer
     * @return 0 on success
     */
    int bt_manager_get_stats(bt_stats_t *stats);

    /**
     * @brief Check if Bluetooth is connected
     * @return true if connected
     */
    bool bt_manager_is_connected(void);

    /**
     * @brief Register event callback
     * @param callback Callback function
     * @param user_data User data
     * @return 0 on success
     */
    int bt_manager_register_callback(bt_event_callback_t callback, void *user_data);

    /**
     * @brief Delete all bonded devices
     * @return 0 on success
     */
    int bt_manager_unpair_all(void);

    /**
     * @brief Get device address string
     * @param buffer Output buffer
     * @param len Buffer length
     * @return 0 on success
     */
    int bt_manager_get_address(char *buffer, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BT_MANAGER_H */
