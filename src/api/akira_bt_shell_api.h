/**
 * @file akira_bt_shell_api.h
 * @brief BT Shell API declarations for WASM exports
 */

#ifndef AKIRA_BT_SHELL_API_H
#define AKIRA_BT_SHELL_API_H

#include <wasm_export.h>
#include <stdint.h>
#include <stddef.h>

/* Core BT Shell API functions (no security checks) */
int akira_bt_shell_send(const char *message);
int akira_bt_shell_send_data(const uint8_t *data, size_t len);
int akira_bt_shell_is_ready(void);

/* WASM native export functions (with capability checks) */
int akira_native_bt_shell_send(wasm_exec_env_t exec_env, const char *message);
int akira_native_bt_shell_send_data(wasm_exec_env_t exec_env, uint32_t data_ptr, uint32_t len);
int akira_native_bt_shell_is_ready(wasm_exec_env_t exec_env);

#endif /* AKIRA_BT_SHELL_API_H */