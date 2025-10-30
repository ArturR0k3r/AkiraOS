/**
 * @file bluetooth_manager.h
 * @brief AkiraOS Bluetooth/BLE Manager API
 */

#ifndef AKIRA_BLUETOOTH_MANAGER_H
#define AKIRA_BLUETOOTH_MANAGER_H

#include <stddef.h>
#include <stdint.h>

void bluetooth_manager_init(void);
void bluetooth_manager_start_advertising(void);
void bluetooth_manager_stop_advertising(void);
void bluetooth_manager_on_connect(void);
void bluetooth_manager_on_disconnect(void);
void bluetooth_manager_send_shell_output(const char *output);
void bluetooth_manager_receive_shell_command(const char *cmd);
void bluetooth_manager_send_ota_update(const uint8_t *data, size_t len);

#endif // AKIRA_BLUETOOTH_MANAGER_H
