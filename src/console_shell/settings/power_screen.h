/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file power_screen.h
 * @brief Battery status and auto power-off.
 */

#ifndef SETTINGS_POWER_SCREEN_H
#define SETTINGS_POWER_SCREEN_H

#include "ui/list_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Settings → Power & battery. */
extern const list_model_t power_page;

/** Full-screen battery details (also opened from the home status area). */
void battery_screen_open(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_POWER_SCREEN_H */
