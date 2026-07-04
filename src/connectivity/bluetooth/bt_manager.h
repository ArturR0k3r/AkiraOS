/**
 * @file bt_manager.h
 * @brief Bluetooth Manager for AkiraOS
 *
 * Manages Bluetooth stack initialization, advertising, connections,
 * and coordinates BLE services (HID, OTA, Shell).
 * @stability experimental
 * @since 1.4
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
        BT_SERVICE_HID       = 0x01,
        BT_SERVICE_OTA       = 0x02,
        BT_SERVICE_COMPANION = 0x04, /**< BLE Companion (AkiraApp) service */
        BT_SERVICE_ALL       = 0x07
    } bt_service_t;

    /**
     * @brief Bluetooth operating mode.
     *
     * Only one mode may be active at a time.  HID mode is activated by the
     * SYS_INIT hook when CONFIG_AKIRA_BT_HID is set.  BLE_APP mode is
     * activated by a WASM app calling ble_init() and is mutually exclusive
     * with HID mode.
     */
    typedef enum
    {
        BT_MODE_NONE      = 0, /**< Stack enabled but no service active */
        BT_MODE_HID       = 1, /**< BLE HID profile owns the radio */
        BT_MODE_BLE_APP   = 2, /**< WASM BLE app service owns the radio */
        BT_MODE_COMPANION = 3, /**< AkiraApp companion service owns the radio */
        BT_MODE_BLE_SCAN  = 4, /**< WASM BLE observer (scan) owns the radio */
        BT_MODE_BLE_SPAM  = 5, /**< WASM BLE rotating-advertiser owns the radio */
    } bt_manager_mode_t;

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

    /* Sizing for struct ble_scan_report. Declared as named constants so the
     * WASM-side wire struct (akira_ble_scan_report_t in AkiraSDK/include/akira_api.h)
     * can be kept in sync deliberately rather than via magic-number duplication. */
    #define BLE_SCAN_ADDR_LEN    6   /**< BLE MAC length */
    #define BLE_SCAN_NAME_LEN    20  /**< local name buffer, NUL-terminated */
    #define BLE_SCAN_ADV_MAX_LEN 31  /**< legacy advertising max payload */

    /**
     * @brief One BLE advertisement report, as delivered by bt_manager_scan_pop().
     *
     * All members are 1-byte aligned (no padding), so this struct can be
     * memcpy'd directly into a WASM buffer with a matching layout.
     */
    struct ble_scan_report
    {
        uint8_t  addr[BLE_SCAN_ADDR_LEN];     /**< Advertiser MAC, as reported by the stack */
        int8_t   rssi;                        /**< Signal strength in dBm */
        char     name[BLE_SCAN_NAME_LEN];     /**< NUL-terminated local name, empty if not advertised */
        uint8_t  adv_len;                     /**< Bytes valid in adv_data */
        uint8_t  adv_data[BLE_SCAN_ADV_MAX_LEN]; /**< Raw AD payload */
    };

    /** BLE spam/spoof presets — publicly-documented advertisement formats
     *  used to trigger nearby-device pairing UI, same technique as
     *  open-source BLE-spam tools. */
    #define BLE_SPAM_PRESET_APPLE     0 /**< Apple Continuity proximity-pair popup */
    #define BLE_SPAM_PRESET_FASTPAIR  1 /**< Google Fast Pair */
    #define BLE_SPAM_PRESET_SWIFTPAIR 2 /**< Microsoft Swift Pair */
    #define BLE_SPAM_PRESET_RANDOM    3 /**< Randomized manufacturer-data flood */
    #define BLE_SPAM_PRESET_COUNT     (BLE_SPAM_PRESET_RANDOM + 1)

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

    /**
     * @brief Set the current BT operating mode.
     *
     * Enforces mutual exclusion: returns -EBUSY if the stack is already
     * locked by the other mode.  Calling with BT_MODE_NONE releases the
     * lock so the other mode may start.
     *
     * @param mode Requested mode
     * @return 0 on success, -EBUSY if another mode is active
     */
    int bt_manager_set_mode(bt_manager_mode_t mode);

    /**
     * @brief Get the current BT operating mode.
     * @return Current mode
     */
    bt_manager_mode_t bt_manager_get_mode(void);

/** NVS key persisting the boot-time BLE mode ("hid" | "companion"). */
#define AKIRA_BT_MODE_KEY "akira/bt/mode"

    /**
     * @brief Whether the persisted boot BLE mode selects the Companion service.
     *
     * Reads @ref AKIRA_BT_MODE_KEY from settings. HID and Companion are mutually
     * exclusive (single BLE connection); this decides which one claims the radio
     * at boot. Returns false (→ HID) when the key is unset, so existing units
     * keep their current behaviour.
     *
     * @return true if the boot mode is "companion", false otherwise.
     */
    bool bt_manager_boot_mode_is_companion(void);

    /**
     * @brief Start advertising with a custom 128-bit service UUID.
     *
     * Used by BLE_APP mode — provides a generic connectable advertisement
     * with the provided 128-bit service UUID, no HID-specific data.
     *
     * @param svc_uuid128 16-byte little-endian UUID (or NULL for generic adv)
     * @return 0 on success, negative errno on failure
     */
    int bt_manager_start_advertising_custom(const uint8_t svc_uuid128[16]);

    /**
     * @brief Start BLE observer scanning. Mutually exclusive with other modes.
     * @param active  true = active scan (sends scan requests, gets names), false = passive.
     * @return 0 on success, -EBUSY if another mode owns the radio.
     */
    int bt_manager_scan_start(bool active);

    /**
     * @brief Stop BLE observer scanning and release the mode lock.
     * @return 0 on success.
     */
    int bt_manager_scan_stop(void);

    /**
     * @brief Pop the next queued advertisement report (non-blocking).
     * @param out  Output report (caller allocates).
     * @return 1 if a report was popped, 0 if the queue is empty, negative errno on error.
     */
    int bt_manager_scan_pop(struct ble_scan_report *out);

    /**
     * @brief Start rotating-advertiser spam/spoof mode.
     * @param preset  One of BLE_SPAM_PRESET_*.
     * @return 0 on success, -EINVAL for unknown preset, -EBUSY if another mode owns the radio.
     */
    int bt_manager_spam_start(int preset);

    /**
     * @brief Stop spam/spoof mode and release the mode lock.
     * @return 0 on success.
     * @note Must not be called from the Zephyr system workqueue thread (blocks synchronously on work cancellation).
     */
    int bt_manager_spam_stop(void);

    /**
     * @brief Number of advertisement payloads sent since the last spam_start().
     * @return Packet count (resets to 0 on each bt_manager_spam_start()).
     */
    uint32_t bt_manager_spam_packet_count(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BT_MANAGER_H */
