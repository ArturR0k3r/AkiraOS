/**
 * @file akira_common_api.h
 * @brief Common API declarations for WASM exports
 * @stability stable
 * @since 1.0
 */

#ifndef AKIRA_COMMON_API_H
#define AKIRA_COMMON_API_H

#include <wasm_export.h>
#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <stddef.h>

/* WASM native export functions */
int akira_native_printf(wasm_exec_env_t exec_env, char *message);

/**
 * delay(us): sleep on behalf of the calling app. Ends early when the app is
 * being stopped; delay(0) yields.
 */
int akira_native_delay(wasm_exec_env_t exec_env, uint32_t microseconds);

/**
 * Print app output on behalf of the calling app at log level @p level
 * (LOG_LEVEL_ERR..LOG_LEVEL_DBG): one log line per text line, prefixed with
 * the app name, lines cut at 160 characters, control characters replaced
 * with '.', and rate-limited per app slot
 * (token bucket: 2 KB burst, 1 KB/s, shared by every output path) so no app
 * can flood the console. Output over budget is dropped and the gap reported.
 * @p buf must already be validated against the app's memory; the caller
 * bounds @p len.
 */
void akira_app_log_write(wasm_exec_env_t exec_env, int level, const char *buf, size_t len);

/** akira_app_log_write() at INFO: printf_native and WASI stdout/stderr. */
void akira_app_console_write(wasm_exec_env_t exec_env, const char *buf, size_t len);

/**
 * Copy the calling app's name into @p buf without taking the runtime lock
 * (unlike akira_runtime_get_name_for_module_inst()), so natives called in a
 * loop never hold that lock when a stop aborts the app.
 *
 * @return 0, or -ENOENT if the caller is not a running app (buf = "").
 */
int akira_app_self_name(wasm_exec_env_t exec_env, char *buf, size_t len);
#endif

#endif /* AKIRA_COMMON_API_H */
