/**
 * @file bt_shell.h
 * @brief Bluetooth Shell Service for AkiraOS
 *
 * Provides a GATT service for sending shell commands to a connected
 * BLE device (phone/tablet). The phone app can subscribe to notifications
 * to receive commands sent from the device's shell.
 *
 * Service UUID: d5b1b7e2-7f5a-4eef-8fd0-1a2b3c4d5e71
 * Shell TX Characteristic: d5b1b7e3-7f5a-4eef-8fd0-1a2b3c4d5e72 (Notify)
 * Shell RX Characteristic: d5b1b7e4-7f5a-4eef-8fd0-1a2b3c4d5e73 (Write)
 */

#ifndef AKIRA_BT_SHELL_H
#define AKIRA_BT_SHELL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize BT Shell service
     * @return 0 on success, negative error code on failure
     */
    int bt_shell_init(void);

    /**
     * @brief Send a shell command to the connected BLE device
     *
     * This sends a command string via BLE notification to the connected
     * phone/tablet. The receiving app should execute or display the command.
     *
     * @param cmd Command string to send (null-terminated)
     * @return 0 on success, -ENOTCONN if not connected, negative error on failure
     */
    int bt_shell_send_command(const char *cmd);

    /**
     * @brief Send raw data to the connected BLE device
     *
     * @param data Data buffer to send
     * @param len Length of data
     * @return 0 on success, negative error code on failure
     */
    int bt_shell_send_data(const uint8_t *data, size_t len);

    /**
     * @brief Check if shell notifications are enabled by the peer
     * @return true if notifications are subscribed, false otherwise
     */
    bool bt_shell_notifications_enabled(void);

    /**
     * @brief Callback type for received shell responses from phone
     * @param data Received data buffer
     * @param len Length of received data
     */
    typedef void (*bt_shell_rx_callback_t)(const uint8_t *data, size_t len);

    /**
     * @brief Register callback for received shell data from phone
     * @param callback Callback function (NULL to unregister)
     */
    void bt_shell_register_rx_callback(bt_shell_rx_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BT_SHELL_H */
