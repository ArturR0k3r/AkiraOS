/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file install_progress_screen.h
 * @brief Modal overlay showing app install progress (BLE / SD / HTTP).
 */

#ifndef INSTALL_PROGRESS_SCREEN_H
#define INSTALL_PROGRESS_SCREEN_H

#if defined(CONFIG_LVGL)
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Show (or update) the install progress overlay.
 *
 * Safe to call from any thread — marshals to the LVGL thread via a
 * k_msgq if called from an ISR or background thread.
 *
 * @param name  App name being installed (may be NULL).
 * @param pct   Progress 0–100.
 * @param msg   Short status text (may be NULL).
 */
void install_progress_show(const char *name, int pct, const char *msg);

/**
 * @brief Remove the install progress overlay.
 */
void install_progress_hide(void);

#ifdef __cplusplus
}
#endif

#endif /* INSTALL_PROGRESS_SCREEN_H */
