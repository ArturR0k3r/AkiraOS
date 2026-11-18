/*
 * AkiraConsole Simulator — WAMR native symbol registration
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef WAMR_HOST_H
#define WAMR_HOST_H

#include <stdbool.h>

/**
 * @brief Register all AkiraOS native symbols with the WAMR runtime.
 *
 * Must be called after wasm_runtime_init() and before any module is loaded.
 * @return true on success.
 */
bool wamr_host_register_natives(void);

#endif /* WAMR_HOST_H */
