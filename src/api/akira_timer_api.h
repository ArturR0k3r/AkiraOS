/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_timer_api.h
 * @brief Polling timer API for WASM applications
 *
 * Provides a pool of timer handles backed by k_uptime_get().
 * No callbacks — apps poll elapsed time and call delay() to yield.
 * All handles are process-scoped: a WASM app cannot access timers
 * created by another app instance.
 */

#ifndef AKIRA_TIMER_API_H
#define AKIRA_TIMER_API_H

#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the timer pool (called once at system init).
 * @return 0 on success.
 */
int akira_timer_init(void);

#ifdef CONFIG_AKIRA_WASM_RUNTIME

/**
 * @brief Allocate a new timer handle.
 * @return Handle index ≥0, or -ENOMEM if pool is exhausted.
 */
int akira_native_timer_create(wasm_exec_env_t exec_env);

/**
 * @brief Start (or restart) a timer.
 * @param handle Handle returned by timer_create().
 * @return 0, -EINVAL for bad handle, -EPERM if not owner.
 */
int akira_native_timer_start(wasm_exec_env_t exec_env, int32_t handle);

/**
 * @brief Stop a running timer, preserving elapsed time.
 * @param handle Handle returned by timer_create().
 * @return 0, -EINVAL, or -EPERM.
 */
int akira_native_timer_stop(wasm_exec_env_t exec_env, int32_t handle);

/**
 * @brief Return elapsed milliseconds.
 * Running timer: now - start_ms.
 * Stopped timer: stopped_ms - start_ms.
 * @return Elapsed ms (int32_t, wraps after ~24 days), or -EINVAL / -EPERM.
 */
int akira_native_timer_elapsed(wasm_exec_env_t exec_env, int32_t handle);

/**
 * @brief Release a timer handle back to the pool.
 * @return 0, -EINVAL, or -EPERM.
 */
int akira_native_timer_free(wasm_exec_env_t exec_env, int32_t handle);

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_TIMER_API_H */
