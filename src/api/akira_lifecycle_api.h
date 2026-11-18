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
 * @stability stable
 * @since 1.4
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
 * Requires AKIRA_CAP_APP_CONTROL.  app_manager_start() (binary load, WAMR
 * instance, thread) runs on the lifecycle work queue, never on the caller's
 * thread; the caller waits for its result, so this still returns once the
 * app's WAMR instantiation has succeeded or failed.  The wait ends early
 * with -EINTR if the caller is stopped (the start still completes).
 *
 * @param exec_env WASM execution environment
 * @param name     Null-terminated app name
 * @return 0 on success, negative Zephyr errno on failure
 *   -EINVAL  empty/NULL/over-long name, or caller tried to start self
 *   -ENOENT  app not installed
 *   -EBUSY   max concurrent apps already running, or too many lifecycle
 *            requests pending
 *   -EAGAIN  this app is sending lifecycle requests too fast (a burst of 4,
 *            then one per 2 s)
 *   -EINTR   the caller is being stopped
 *   -EPERM   capability not granted
 */
int akira_native_app_start(wasm_exec_env_t exec_env, const char *name);

/**
 * @brief Stop a running app by name.
 *
 * Requires AKIRA_CAP_APP_CONTROL.  An app cannot stop itself via this API;
 * self-exit is done by returning 0 from main().  Like app_start, the stop
 * runs on the lifecycle work queue (it can take up to the stop timeout) and
 * the caller waits for the result.
 *
 * @param exec_env WASM execution environment
 * @param name     Null-terminated app name
 * @return 0 on success, negative Zephyr errno on failure
 *   -EINVAL  empty/NULL/over-long name, or caller tried to stop self
 *   -ENOENT  app not installed
 *   -EBUSY / -EAGAIN / -EINTR  as for akira_native_app_start()
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
 *         (-EAGAIN: lifecycle requests too fast, see akira_native_app_start())
 */
int akira_native_app_switch(wasm_exec_env_t exec_env, const char *name);

/**
 * @brief Check whether a newer version of the calling app is available.
 * Registered "(*~)i": WAMR validates [ver_buf, ver_buf + buf_len).
 * Reads cached state only (no network round-trip from this call) — kept
 * fresh via Hub push (MSG_TYPE_APP_AVAILABLE) and periodic polling.
 * @param ver_buf  Output buffer for the available version string
 * @param buf_len  Buffer capacity
 * @return 1 update available, 0 up to date, negative errno
 */
int akira_native_app_check_update(wasm_exec_env_t exec_env, uint8_t *ver_buf, uint32_t buf_len);

/**
 * @brief Fire-and-forget: trigger download+install-in-place of the calling
 * app's own newer version. App should expect to be stopped and reinstalled
 * when it lands (app_manager_install() already handles same-name reinstall).
 * @return 0 queued, negative errno (no update available, no connectivity)
 */
int akira_native_app_request_update(wasm_exec_env_t exec_env);

/** A lifecycle operation on an app, e.g. app_manager_start(). */
typedef int (*akira_lifecycle_op_t)(const char *name);

/**
 * @brief Run @p op(@p name) on the lifecycle work queue for the calling app.
 *
 * For work a native must not do on an app's thread because it holds the app
 * registry or filesystem locks for long, or waits for another app (an abort
 * in the middle would leak the lock). The name is copied; the caller waits
 * for the result in stop-aware sleeps. If the caller is stopped, or after
 * 60 s, it returns -EINTR / -ETIMEDOUT and the call still completes on the
 * queue. Shares the per-app rate limit of app_start/app_stop/app_switch.
 *
 * @return @p op's result, or -EINVAL (bad name), -EPERM (not an app),
 *         -EAGAIN (rate limit), -EBUSY (queue full), -EINTR, -ETIMEDOUT,
 *         -ENOTSUP (no app manager)
 */
int akira_lifecycle_run_deferred(wasm_exec_env_t exec_env, akira_lifecycle_op_t op,
                                 const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_LIFECYCLE_API_H */
