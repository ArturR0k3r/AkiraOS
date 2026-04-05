/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file akira_os_shell.h
 * @brief AkiraConsole Native OS Shell — public API
 *
 * The OS shell is a Zephyr APPLICATION-level SYS_INIT module that owns the
 * display at boot and provides:
 *   - App launcher (home_screen)
 *   - App installation from SD card, BLE, and HTTP (sd_install_screen,
 *     install_progress_screen)
 *   - System settings (settings_screen and sub-screens)
 *   - Display ownership arbitration with WASM apps
 *   - HOME button (X, GPIO17) handler to return from any running WASM app
 */

#ifndef AKIRA_OS_SHELL_H
#define AKIRA_OS_SHELL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Notify the OS shell of install progress (called from connectivity layer).
 *
 * Thread-safe: may be called from any thread (BLE transfer thread, HTTP
 * server thread, etc.).  The shell posts the update to its own display
 * thread via a message queue.
 *
 * @param name      App name being installed (may be NULL if unknown yet).
 * @param pct       Progress percent 0–100.  Pass 100 on completion.
 * @param msg       Short status string (may be NULL).  Truncated to 63 chars.
 */
void akira_os_shell_notify_install_progress(const char *name, int pct,
                                            const char *msg);

/**
 * @brief Navigate the OS shell to the HOME screen.
 *
 * Safe to call from any thread including ISR context (uses k_msgq).
 * If a WASM app is running it is stopped first.
 */
void akira_os_shell_go_home(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_OS_SHELL_H */
