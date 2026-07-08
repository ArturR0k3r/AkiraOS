/**
 * @file bt_echo.h
 * @brief Simple Bluetooth Echo GATT service (for testing)
 * @stability experimental
 * @since 1.4
 */

#ifndef AKIRA_BT_ECHO_H
#define AKIRA_BT_ECHO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize echo service (no-op if BT not enabled) */
int bt_echo_init(void);

/** Enable/disable echo behavior at runtime */
void bt_echo_enable(bool enable);
bool bt_echo_is_enabled(void);

/**
 * Proactively send @p data to connected peer(s) via an echo-characteristic
 * notification. Returns 0 on success, negative errno on failure
 * (e.g. -ENOTSUP if BT is not enabled).
 */
int bt_echo_send(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BT_ECHO_H */
