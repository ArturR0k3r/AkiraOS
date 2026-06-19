/**
 * @file akira_rf_api.h
 * @brief RF API declarations for WASM exports
 * @stability experimental
 * @since 1.4
 */

#ifndef AKIRA_RF_API_H
#define AKIRA_RF_API_H

#include <stdint.h>
#include <stddef.h>
#include <wasm_export.h>
#include "connectivity/radio_interface.h"


/* RF chip types */
typedef enum {
    AKIRA_RF_CHIP_NONE = 0,
    AKIRA_RF_CHIP_NRF24L01,
    AKIRA_RF_CHIP_CC1101,
    AKIRA_RF_CHIP_LR1121,
    AKIRA_RF_CHIP_CC1121,
    AKIRA_RF_CHIP_LR2021,
    AKIRA_RF_CHIP_MAX,
} akira_rf_chip_t;

/* Core RF API functions (no security checks) */
int akira_rf_init(akira_rf_chip_t chip);
int akira_rf_deinit(void);
/* Make `chip` the active RF chip. Hardware-inits it on first select, then just
 * flips the active pointer on later switches, preserving each chip's config. */
int akira_rf_select(akira_rf_chip_t chip);
/* Pop one packet from the background RX queue. timeout_ms=0 is non-blocking.
 * Returns bytes copied, -ENOMSG if empty, -ENOSYS if daemon disabled. */
int akira_rf_recv_pop(uint8_t *buf, size_t max_len, uint32_t timeout_ms);
int akira_rf_send(const uint8_t *data, size_t len);
int akira_rf_receive(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);
int akira_rf_set_frequency(uint32_t freq_hz);
int akira_rf_set_power(int8_t dbm);
int akira_rf_set_modulation(radio_modulation_t mod);
int akira_rf_set_spreading_factor(uint8_t sf);
int akira_rf_set_bandwidth(uint32_t bw_hz);
int akira_rf_set_coding_rate(uint8_t cr);
int akira_rf_get_rssi(int16_t *rssi);
radio_handle_t *akira_rf_get_active_handle(void);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM native export functions (with capability checks) */
int akira_native_rf_select(wasm_exec_env_t exec_env, int chip);
int akira_native_rf_recv_pop(wasm_exec_env_t exec_env, uint32_t buf_ptr, uint32_t max_len, uint32_t timeout_ms);
int akira_native_rf_send(wasm_exec_env_t exec_env, uint32_t payload_ptr, uint32_t len);
int akira_native_rf_receive(wasm_exec_env_t exec_env, uint32_t buffer_ptr, uint32_t max_len, uint32_t timeout_ms);
int akira_native_rf_set_frequency(wasm_exec_env_t exec_env, uint32_t freq_hz);
int akira_native_rf_get_rssi(wasm_exec_env_t exec_env);
int akira_native_rf_set_power(wasm_exec_env_t exec_env, int8_t dbm);
int akira_native_rf_set_modulation(wasm_exec_env_t exec_env, int mod);
int akira_native_rf_set_spreading_factor(wasm_exec_env_t exec_env, int sf);
int akira_native_rf_set_bandwidth(wasm_exec_env_t exec_env, uint32_t bw_hz);
int akira_native_rf_set_coding_rate(wasm_exec_env_t exec_env, int cr);

#if defined(CONFIG_WIFI) && defined(CONFIG_AKIRA_RF_FRAMEWORK)
/* WiFi spectrum scan (per-channel max RSSI) */
int akira_native_wifi_scan_rssi(wasm_exec_env_t exec_env,
                                 uint32_t buf_ptr, uint32_t buf_len);
/* WiFi AP scan — full records: SSID/BSSID/channel/RSSI/security */
int akira_native_wifi_scan_aps(wasm_exec_env_t exec_env,
                                void *buf, uint32_t buf_len);
/* 802.11 deauthentication frame injector (requires wifi.inject capability) */
int akira_native_wifi_deauth(wasm_exec_env_t exec_env,
                              void *bssid_ptr, void *client_ptr,
                              int32_t channel, int32_t count, int32_t interval_ms);
#endif

/*
 * Raw Sub-GHz OOK/ASK capture and replay.
 * Both functions require AKIRA_CAP_RF_TRANSCEIVE.
 * The chip must be selected via rf_select() and configured via
 * rf_set_frequency() / rf_set_modulation() before calling these.
 *
 * Pulse buffer layout: uint16_t[], alternating mark/space durations in µs.
 * Index 0 = first mark (high), index 1 = first space (low), ...
 */

/* Capture raw OOK pulse timings.  Type string: "(iii)i" */
int akira_native_rf_raw_capture(wasm_exec_env_t exec_env,
                                 uint32_t buf_wasm, uint32_t max_samples,
                                 int32_t timeout_ms);

/* Replay a raw OOK pulse sequence.  Type string: "(iii)i" */
int akira_native_rf_raw_replay(wasm_exec_env_t exec_env,
                                uint32_t buf_wasm, uint32_t sample_count,
                                int32_t repeat);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_RF_API_H */