/**
 * @file akira_input_api.h
 * @brief Input API declarations for WASM exports
 */

#ifndef AKIRA_INPUT_API_H
#define AKIRA_INPUT_API_H

#include <wasm_export.h>
#include <stdint.h>

/* Input callback type */
typedef void (*akira_input_callback_t)(uint32_t buttons);

/* Core input API functions (no security checks) */
int akira_input_read_buttons(void);
int akira_input_button_pressed(uint32_t button);
int akira_input_set_callback(akira_input_callback_t callback);
void akira_input_notify(uint32_t buttons);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM native export functions (with capability checks) */
int akira_native_input_read_buttons(wasm_exec_env_t exec_env);
int akira_native_input_button_pressed(wasm_exec_env_t exec_env, uint32_t button);
int akira_native_input_set_callback(wasm_exec_env_t exec_env, akira_input_callback_t callback);
int akira_native_input_notify(wasm_exec_env_t exec_env, uint32_t buttons);
#endif

#endif /* AKIRA_INPUT_API_H */