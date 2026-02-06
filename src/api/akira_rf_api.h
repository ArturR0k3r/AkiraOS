/**
 * @file akira_rf_api.h
 * @brief RF API declarations for WASM exports
 */

#ifndef AKIRA_RF_API_H
#define AKIRA_RF_API_H

#include <stdint.h>
#include <stddef.h>
#include <wasm_export.h>


/* RF chip types */
typedef enum {
    AKIRA_RF_CHIP_NONE = 0,
    AKIRA_RF_CHIP_NRF24L01,
    AKIRA_RF_CHIP_CC1101,
    AKIRA_RF_CHIP_LR1121,
} akira_rf_chip_t;

/* Core RF API functions (no security checks) */
int akira_rf_init(akira_rf_chip_t chip);
int akira_rf_deinit(void);
int akira_rf_send(const uint8_t *data, size_t len);
int akira_rf_receive(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);
int akira_rf_set_frequency(uint32_t freq_hz);
int akira_rf_set_power(int8_t dbm);
int akira_rf_get_rssi(int16_t *rssi);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM native export functions (with capability checks) */
int akira_native_rf_send(wasm_exec_env_t exec_env, uint32_t payload_ptr, uint32_t len);
int akira_native_rf_receive(wasm_exec_env_t exec_env, uint32_t buffer_ptr, uint32_t max_len, uint32_t timeout_ms);
int akira_native_rf_set_frequency(wasm_exec_env_t exec_env, uint32_t freq_hz);
int akira_native_rf_get_rssi(wasm_exec_env_t exec_env, int16_t *rssi);
int akira_native_rf_set_power(wasm_exec_env_t exec_env, int8_t dbm);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_RF_API_H */