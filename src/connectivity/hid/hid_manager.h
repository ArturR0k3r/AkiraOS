/**
 * @file hid_manager.h
 * @brief HID Manager for AkiraOS
 *
 * Unified HID device management supporting keyboard and gamepad profiles
 * over multiple transports (BLE, USB, Simulation).
 */

#ifndef AKIRA_HID_MANAGER_H
#define AKIRA_HID_MANAGER_H

#include "hid_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /*===========================================================================*/
    /* Configuration                                                             */
    /*===========================================================================*/

    /** HID manager configuration */
    typedef struct
    {
        hid_device_type_t device_types;      /**< Which HID device types to enable */
        hid_transport_t preferred_transport; /**< Preferred transport */
        const char *device_name;             /**< Device name for discovery */
        uint16_t vendor_id;                  /**< USB/BLE vendor ID */
        uint16_t product_id;                 /**< USB/BLE product ID */
    } hid_config_t;

    /*===========================================================================*/
    /* HID Manager API                                                           */
    /*===========================================================================*/

    /**
     * @brief Initialize HID manager
     * @param config Configuration options
     * @return 0 on success, negative error code on failure
     */
    int hid_manager_init(const hid_config_t *config);

    /**
     * @brief Deinitialize HID manager
     * @return 0 on success
     */
    int hid_manager_deinit(void);

    /**
     * @brief Enable HID device
     *
     * Starts advertising (BLE) or enables USB endpoint
     * @return 0 on success
     */
    int hid_manager_enable(void);

    /**
     * @brief Disable HID device
     * @return 0 on success
     */
    int hid_manager_disable(void);

    /**
     * @brief Select active transport
     * @param transport Transport to use
     * @return 0 on success
     */
    int hid_manager_set_transport(hid_transport_t transport);

    /**
     * @brief Get current transport
     * @return Current transport type
     */
    hid_transport_t hid_manager_get_transport(void);

    /**
     * @brief Check if HID is connected to host
     * @return true if connected
     */
    bool hid_manager_is_connected(void);

    /**
     * @brief Get current HID state
     * @return Pointer to state structure
     */
    const hid_state_t *hid_manager_get_state(void);

    /*===========================================================================*/
    /* Keyboard API                                                              */
    /*===========================================================================*/

    /**
     * @brief Press a key
     * @param key HID key code
     * @return 0 on success
     */
    int hid_keyboard_press(hid_key_code_t key);

    /**
     * @brief Release a key
     * @param key HID key code
     * @return 0 on success
     */
    int hid_keyboard_release(hid_key_code_t key);

    /**
     * @brief Release all keys
     * @return 0 on success
     */
    int hid_keyboard_release_all(void);

    /**
     * @brief Set modifier keys
     * @param modifiers Modifier bitmask
     * @return 0 on success
     */
    int hid_keyboard_set_modifiers(uint8_t modifiers);

    /**
     * @brief Type a string (press and release each key)
     * @param str ASCII string to type
     * @param send_callback Optional callback for sending each report
     * @return 0 on success
     */
    int hid_keyboard_type_string(const char *str, int (*send_callback)(const hid_keyboard_report_t *));

    /**
     * @brief Send raw keyboard report
     * @param report Keyboard report
     * @return 0 on success
     */
    int hid_keyboard_send_report(const hid_keyboard_report_t *report);

    /*===========================================================================*/
    /* Gamepad API                                                               */
    /*===========================================================================*/

    /**
     * @brief Press a gamepad button
     * @param button Button bitmask
     * @return 0 on success
     */
    int hid_gamepad_press(hid_gamepad_btn_t button);

    /**
     * @brief Release a gamepad button
     * @param button Button bitmask
     * @return 0 on success
     */
    int hid_gamepad_release(hid_gamepad_btn_t button);

    /**
     * @brief Set gamepad axis value
     * @param axis Axis index
     * @param value Axis value (-32768 to 32767)
     * @return 0 on success
     */
    int hid_gamepad_set_axis(hid_gamepad_axis_t axis, int16_t value);

    /**
     * @brief Set D-pad/hat position
     * @param direction D-pad direction (0-8, 0=center)
     * @return 0 on success
     */
    int hid_gamepad_set_dpad(uint8_t direction);

    /**
     * @brief Send raw gamepad report
     * @param report Gamepad report
     * @return 0 on success
     */
    int hid_gamepad_send_report(const hid_gamepad_report_t *report);

    /**
     * @brief Reset all gamepad inputs to neutral
     * @return 0 on success
     */
    int hid_gamepad_reset(void);

    /*===========================================================================*/
    /* Rate-Limited Sending (Automatic Throttling)                              */
    /*===========================================================================*/

    /**
     * @brief Send keyboard report with rate limiting
     * 
     * Enforces minimum 8ms interval between reports (125Hz max).
     * @return 0 on success, -EAGAIN if too soon, other negative on error
     */
    int hid_keyboard_send_throttled(void);

    /**
     * @brief Send gamepad report with rate limiting
     * 
     * Enforces minimum 8ms interval between reports (125Hz max).
     * @return 0 on success, -EAGAIN if too soon, other negative on error
     */
    int hid_gamepad_send_throttled(void);

    /*===========================================================================*/
    /* Callbacks                                                                 */
    /*===========================================================================*/

    /**
     * @brief Register HID event callback
     * @param callback Callback function
     * @param user_data User data
     * @return 0 on success
     */
    int hid_manager_register_event_callback(hid_event_callback_t callback, void *user_data);

    /**
     * @brief Register output report callback (LED state, rumble, etc.)
     * @param callback Callback function
     * @param user_data User data
     * @return 0 on success
     */
    int hid_manager_register_output_callback(hid_output_callback_t callback, void *user_data);

    /*===========================================================================*/
    /* Transport Registration                                                    */
    /*===========================================================================*/

    /**
     * @brief Register a HID transport
     * @param ops Transport operations
     * @return 0 on success
     */
    int hid_manager_register_transport(const hid_transport_ops_t *ops);

    /**
     * @brief Unregister a HID transport
     * @param name Transport name
     * @return 0 on success
     */
    int hid_manager_unregister_transport(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_HID_MANAGER_H */
