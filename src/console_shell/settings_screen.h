/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file settings_screen.h
 * @brief AkiraConsole OS Shell — Settings screen (pure akira_display_* renderer).
 *
 * Sub-screens: WiFi connect/disconnect, Web Server start/stop, About.
 */

#ifndef SETTINGS_SCREEN_H
#define SETTINGS_SCREEN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize internal state (call once at boot). */
void settings_screen_create(void);

/** Enter the settings screen from the home launcher. */
void settings_screen_load(void);

/** Feed a just-pressed button bitmask (BIT(AKIRA_BTN_*) flags). */
void settings_screen_handle_key(uint32_t just_pressed);

/** Periodic refresh — call every 1 s from the shell timer tick. */
void settings_screen_update(void);

/** Return true while the settings screen owns the display. */
bool settings_screen_is_active(void);

/** Return true while the display is blanked (sleep mode). */
bool settings_screen_is_sleeping(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_SCREEN_H */
