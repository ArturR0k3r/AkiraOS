/**
 * @file bt_echo.h
 * @brief Simple Bluetooth Echo GATT service (for testing)
 * @stability experimental
 * @since 1.4
 */

#ifndef AKIRA_BT_ECHO_H
#define AKIRA_BT_ECHO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize echo service (no-op if BT not enabled) */
int bt_echo_init(void);

/** Enable/disable echo behavior at runtime */
void bt_echo_enable(bool enable);
bool bt_echo_is_enabled(void);

/** Send data to all connected peers via BT Echo notification */
int bt_echo_send(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BT_ECHO_H */
