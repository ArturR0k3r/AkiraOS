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
/* Release radio-manager ownership of all RF-held chips WITHOUT powering them
 * down (no ops->deinit). Lets the mesh acquire the LR2021 that main() inits at
 * boot, avoiding the deinit-induced BUSY/boot timeout. */
int akira_rf_release_all(void);
/* Make `chip` the active RF chip. Hardware-inits it on first select, then just
 * flips the active pointer on later switches, preserving each chip's config. */
int akira_rf_select(akira_rf_chip_t chip);
/* Pop one packet from the background RX queue. timeout_ms=0 is non-blocking.
 * Returns bytes copied, -ENOMSG if empty, -ENOSYS if daemon disabled. */
int akira_rf_recv_pop(uint8_t *buf, size_t max_len, uint32_t timeout_ms);
/* Stateless raw OOK capture/replay (shell + WASM both call these). */
int akira_rf_raw_capture(uint8_t *buf, size_t max_bytes,
                         uint32_t sample_rate_hz, uint32_t timeout_ms);
int akira_rf_raw_replay(const uint8_t *buf, size_t len,
                        uint32_t sample_rate_hz, uint32_t repeat);
int akira_rf_send(const uint8_t *data, size_t len);
int akira_rf_receive(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);
int akira_rf_set_frequency(uint32_t freq_hz);
int akira_rf_set_power(int8_t dbm);
int akira_rf_set_bitrate(uint32_t bps);
int akira_rf_set_modulation(radio_modulation_t mod);
int akira_rf_set_spreading_factor(uint8_t sf);
int akira_rf_set_bandwidth(uint32_t bw_hz);
int akira_rf_set_coding_rate(uint8_t cr);
int akira_rf_get_rssi(int16_t *rssi);
radio_handle_t *akira_rf_get_active_handle(void);

/** True while the RF RX daemon is actively listening on the active radio. */
bool akira_rf_daemon_is_running(void);
/** Put the active radio into RADIO_MODE_SLEEP if it is idle. No-op if busy or absent. */
int akira_rf_sleep_if_idle(void);
/** Restore the active radio to RADIO_MODE_STANDBY after akira_rf_sleep_if_idle(). */
int akira_rf_wake(void);

/* Continuous-wave (CW) TX for jamming / range testing. */
int akira_rf_tx_cw_start(void);
int akira_rf_tx_cw_stop(void);
int akira_rf_tx_cw_set_freq(uint32_t freq_hz);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM native export functions (with capability checks) */
int akira_native_rf_select(wasm_exec_env_t exec_env, int chip);
int akira_native_rf_recv_pop(wasm_exec_env_t exec_env, uint32_t buf_ptr, uint32_t max_len, uint32_t timeout_ms);
int akira_native_rf_send(wasm_exec_env_t exec_env, void *payload, uint32_t len);
int akira_native_rf_receive(wasm_exec_env_t exec_env, uint32_t buffer_ptr, uint32_t max_len, uint32_t timeout_ms);
int akira_native_rf_set_frequency(wasm_exec_env_t exec_env, uint32_t freq_hz);
int akira_native_rf_get_rssi(wasm_exec_env_t exec_env);
int akira_native_rf_set_power(wasm_exec_env_t exec_env, int8_t dbm);
int akira_native_rf_set_modulation(wasm_exec_env_t exec_env, int mod);
int akira_native_rf_set_spreading_factor(wasm_exec_env_t exec_env, int sf);
int akira_native_rf_set_bandwidth(wasm_exec_env_t exec_env, uint32_t bw_hz);
int akira_native_rf_set_coding_rate(wasm_exec_env_t exec_env, int cr);
int akira_native_rf_set_bitrate(wasm_exec_env_t exec_env, int32_t bps);

/* Continuous-wave TX (CW) — type string: "()i" */
int akira_native_rf_tx_cw_start(wasm_exec_env_t exec_env);
int akira_native_rf_tx_cw_stop(wasm_exec_env_t exec_env);
int akira_native_rf_tx_cw_set_freq(wasm_exec_env_t exec_env, uint32_t freq_hz);

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
/* 4-way handshake capture: deauth + sniff EAPOL-Key M1+M2, returns 1 if complete */
int akira_native_wifi_capture_pmkid(wasm_exec_env_t exec_env,
                                     void *bssid_ptr, void *client_ptr,
                                     int32_t channel, const char *ssid_str,
                                     void *result_ptr, int32_t timeout_ms);
/* Client enumeration: passive promiscuous sniff on one channel; writes
 * {mac[6], rssi} records (struct client_wire) into out, returns client count */
int akira_native_wifi_scan_clients(wasm_exec_env_t exec_env,
                                    void *bssid_ptr, void *out_ptr,
                                    uint32_t out_len, int32_t channel,
                                    int32_t timeout_ms);
#endif

/*
 * Raw Sub-GHz OOK capture and replay.
 * Both require AKIRA_CAP_RF_TRANSCEIVE. Chip must be selected (rf_select) and
 * frequency set (rf_set_frequency) first.
 *
 * Buffer = raw demodulated OOK bitstream: 8 samples/byte, sampled at
 * sample_rate_hz. The same sample_rate_hz must be passed to replay to
 * reproduce timing. The native layer does no storage — the app persists the
 * blob {sample_rate, len, bytes} to a file, or replays it transiently.
 */

/* Capture raw OOK bytes into a WASM buffer. Type string: "(iiii)i" */
int akira_native_rf_raw_capture(wasm_exec_env_t exec_env,
                                 uint32_t buf_wasm, uint32_t max_bytes,
                                 uint32_t sample_rate_hz, int32_t timeout_ms);

/* Replay raw OOK bytes from a WASM buffer. Type string: "(iiii)i" */
int akira_native_rf_raw_replay(wasm_exec_env_t exec_env,
                                uint32_t buf_wasm, uint32_t len,
                                uint32_t sample_rate_hz, int32_t repeat);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_RF_API_H */