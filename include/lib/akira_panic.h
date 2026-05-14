/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_panic.h
 * @brief Panic handler — crash capture, NVS storage, next-boot report.
 *
 * Hooks into k_sys_fatal_error_handler().  On each fatal fault:
 *   1. Writes a crash record to CONFIG_AKIRA_PANIC_PARTITION (NVS).
 *   2. Flushes logs.
 *   3. Reboots.
 *
 * On the next boot, akira_panic_init() reads any stored crash report,
 * logs it at ERR level, and clears the partition.
 *
 * Compile-time gate: CONFIG_AKIRA_PANIC=y
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_PANIC_H
#define AKIRA_PANIC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of the "last app" name stored in a crash record. */
#define AKIRA_PANIC_APP_NAME_MAX 32

/**
 * @brief Stored crash record (written to NVS, read on next boot).
 */
typedef struct {
    uint32_t magic;             /**< 0xDEADC0DE when record is valid */
    uint64_t uptime_ms;         /**< k_uptime_get() at crash time */
    uint32_t reason;            /**< k_fatal_error_reason value */
    uintptr_t fault_addr;       /**< Faulting PC / ESF address */
    uintptr_t stack_ptr;        /**< Stack pointer at fault */
    char last_app[AKIRA_PANIC_APP_NAME_MAX]; /**< App running at crash (best-effort) */
} akira_crash_record_t;

/**
 * @brief Initialise panic subsystem and report any previous crash.
 *
 * Call once at early boot (after NVS/settings init).
 * If a crash record exists it is logged via LOG_ERR and then erased.
 *
 * @return 0 on success, negative errno on NVS access failure.
 */
int akira_panic_init(void);

/**
 * @brief Check if a crash record from the previous boot is present.
 *
 * @param[out] out  Filled with the crash record if true is returned.
 * @return true if a record exists (and *out is valid), false otherwise.
 */
bool akira_panic_has_record(akira_crash_record_t *out);

/**
 * @brief Store the name of the currently running app.
 *
 * Call this whenever the active app changes so the panic handler can
 * attribute a crash to the correct app.
 *
 * @param name  App name (copied internally).
 */
void akira_panic_set_active_app(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_PANIC_H */
