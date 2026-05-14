/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_ota_api.h
 * @brief OTA update WASM native API.
 *
 * Allows trusted WASM apps (with AKIRA_CAP_OTA_TRIGGER) to check for,
 * fetch, and apply firmware updates, and to query/confirm/rollback OTA.
 *
 * Gate: CONFIG_AKIRA_WASM_OTA=y (depends on CONFIG_AKIRA_OTA)
 * Capability: AKIRA_CAP_OTA_TRIGGER (bit 31)
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_OTA_API_H
#define AKIRA_OTA_API_H

#include <wasm_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fetch the OTA manifest at the given URL and check if an update
 *        is available for the running firmware version.
 *
 * @param manifest_url  HTTPS URL of the JSON manifest (must start with "https://").
 * @return 1 if update available, 0 if up-to-date, negative errno on error.
 */
int akira_native_ota_check(wasm_exec_env_t exec_env, const char *manifest_url);

/**
 * @brief Download and stage firmware from the last-checked manifest.
 *
 * Blocking call.  Progress is emitted via akira_telemetry_ota_progress().
 * Returns after the image is staged in the secondary slot but NOT yet booted.
 *
 * @param manifest_url  Same URL as passed to akira_native_ota_check().
 * @return 0 on success, negative errno on failure.
 */
int akira_native_ota_fetch_and_apply(wasm_exec_env_t exec_env,
                                      const char *manifest_url);

/**
 * @brief Return the current OTA state (ota_state enum value).
 * @return ota_state cast to int, or -ENODEV if OTA not initialised.
 */
int akira_native_ota_get_state(wasm_exec_env_t exec_env);

/**
 * @brief Confirm the running firmware (clears boot guard counter).
 * @return 0 on success, negative errno on failure.
 */
int akira_native_ota_confirm(wasm_exec_env_t exec_env);

/**
 * @brief Request an immediate rollback to the previous firmware.
 * @return 0 on success (device will reboot), negative errno on failure.
 */
int akira_native_ota_rollback(wasm_exec_env_t exec_env);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_OTA_API_H */
