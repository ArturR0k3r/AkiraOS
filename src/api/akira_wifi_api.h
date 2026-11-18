/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_wifi_api.h
 * @brief WiFi injection/sniffer WASM native API — PMKID capture, client
 *        enumeration and the app-teardown release hook.
 *
 * The passive scan natives (wifi_scan_rssi/wifi_scan_aps) and the deauth
 * injector (wifi_deauth) are declared in akira_rf_api.h and implemented in
 * akira_rf_api.c.  This header covers only the two console-exclusive sniffer
 * natives and the release hook that disarms promiscuous mode when their app
 * ends.
 *
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_WIFI_API_H
#define AKIRA_WIFI_API_H

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#if defined(CONFIG_WIFI) && (defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32))
/* 4-way handshake / PMKID capture (requires wifi.inject capability + Seal). */
int akira_native_wifi_capture_pmkid(wasm_exec_env_t exec_env,
                                     void *bssid_ptr, void *client_ptr,
                                     int32_t channel, const char *ssid_str,
                                     void *result_ptr, int32_t timeout_ms);
/* Associated-client enumeration (requires wifi.inject capability + Seal). */
int akira_native_wifi_scan_clients(wasm_exec_env_t exec_env,
                                    void *bssid_ptr, void *out_ptr,
                                    uint32_t out_len, int32_t channel,
                                    int32_t timeout_ms);
#endif
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/**
 * @brief Release WiFi injection state left behind by an app at teardown.
 *
 * Disarms the handshake/client sniffers, drops promiscuous mode and the
 * promiscuous RX callback, and hands the radio back to the managed WiFi stack.
 * Only acts for the app slot that armed injection.  Idempotent; safe to call
 * for a slot that owns nothing (and a no-op on non-ESP32 targets).  @p slot is
 * the app slot from akira_runtime_slot_of_exec_env().
 *
 * Registered as an AKIRA_APP_RESOURCE_DEFINE() release hook; the runtime runs
 * it on every app exit path before the app's memory is freed.
 */
void akira_wifi_api_release(int slot);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_WIFI_API_H */
