/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_lifecycle_api.h
 * @brief App Lifecycle WASM API — start, stop, restart, status, list
 *
 * Allows a trusted WASM app to control the lifecycle of other installed apps.
 * Requires AKIRA_CAP_APP_CONTROL — elevated privilege; must not be granted
 * to untrusted apps.
 *
 * Manifest entry:  "capabilities": ["app.control"]
 */

#ifndef AKIRA_LIFECYCLE_API_H
#define AKIRA_LIFECYCLE_API_H

#include <stdint.h>
#include <wasm_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the state of an app as an integer
 * @param exec_env  WASM execution environment
 * @param name      Target app name
 * @return app_state_t value (0-5), negative on error
 *   0=NEW, 1=INSTALLED, 2=RUNNING, 3=STOPPED, 4=ERROR, 5=FAILED
 */
int akira_native_app_get_status(wasm_exec_env_t exec_env, const char *name);

/**
 * @brief List all installed apps into a WASM buffer
 *
 * Writes newline-separated "name:state" entries into the WASM buffer.
 * Example output: "hello_world:RUNNING\ntetris:INSTALLED\n"
 *
 * @param exec_env  WASM execution environment
 * @param buf_ptr   WASM linear-memory pointer to output buffer
 * @param buf_len   Length of the output buffer
 * @return Number of apps written, negative on error
 */
int akira_native_app_list(wasm_exec_env_t exec_env,
                          uint32_t buf_ptr, uint32_t buf_len);

/**
 * @brief Get the calling app's own name
 * @param exec_env  WASM execution environment
 * @param buf_ptr   WASM linear-memory pointer to output buffer
 * @param buf_len   Buffer length (must be >= 32)
 * @return Length of the name string, negative on error
 */
int akira_native_app_get_self_name(wasm_exec_env_t exec_env,
                                   uint32_t buf_ptr, uint32_t buf_len);

/**
 * @brief Start an installed app by name.
 *
 * Requires AKIRA_CAP_APP_CONTROL.  Calls app_manager_start() which loads
 * the binary, creates a WAMR instance, and spawns its thread.  Returns once
 * the app's WAMR instantiation has succeeded or failed.
 *
 * @param exec_env WASM execution environment
 * @param name     Null-terminated app name
 * @return 0 on success, negative Zephyr errno on failure
 *   -EINVAL  empty or NULL name
 *   -ENOENT  app not installed
 *   -EBUSY   max concurrent apps already running
 *   -EPERM   capability not granted
 */
int akira_native_app_start(wasm_exec_env_t exec_env, const char *name);

/**
 * @brief Stop a running app by name.
 *
 * Requires AKIRA_CAP_APP_CONTROL.  An app cannot stop itself via this API;
 * self-exit is done by returning 0 from main().
 *
 * @param exec_env WASM execution environment
 * @param name     Null-terminated app name
 * @return 0 on success, negative Zephyr errno on failure
 *   -EINVAL  empty/NULL name, or caller tried to stop self
 *   -ENOENT  app not installed
 *   -EPERM   capability not granted
 */
int akira_native_app_stop(wasm_exec_env_t exec_env, const char *name);

/**
 * @brief Start another app and signal this app to exit.
 *
 * Requires AKIRA_CAP_APP_SWITCH or AKIRA_CAP_APP_CONTROL.  Starts (or
 * resumes) the target app; the calling WASM app must then return 0 from its
 * own main() to complete the handoff.  The supervisor detects both events
 * via the akira.lifecycle IPC topic.
 *
 * Typical usage:
 * @code
 *   int main(void) {
 *       // ... game loop ...
 *       app_switch("supervisor");  // start/resume supervisor
 *       return 0;                  // clean exit triggers lifecycle event
 *   }
 * @endcode
 *
 * @param exec_env WASM execution environment
 * @param name     Target app name
 * @return 0 on success (caller should return from main), negative on error
 */
int akira_native_app_switch(wasm_exec_env_t exec_env, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_LIFECYCLE_API_H */
