/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file home_screen.h
 * @brief AkiraConsole OS Shell — HOME screen (app launcher).
 *
 * Pure akira_display_* renderer.  No LVGL dependency.
 */

#ifndef HOME_SCREEN_H
#define HOME_SCREEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise home screen state.  Call once at shell startup.
 */
void home_screen_create(void);

/**
 * @brief Reload app list from app_manager and redraw the full screen.
 *
 * Safe to call any time the OS shell holds the display.
 */
void home_screen_refresh(void);

/**
 * @brief Redraw the home screen (same as refresh but does not re-query apps).
 */
void home_screen_load(void);

/**
 * @brief Update the clock / battery strip.  Call every ~1 s from the shell loop.
 */
void home_screen_update_status(void);

/**
 * @brief Animation + dirty-flag tick.  Call every 20 ms from the shell loop.
 *        Internally gates the 1-second status refresh.
 */
void home_screen_tick(void);

/**
 * @brief Forward a button edge-bitmask for carousel / options navigation.
 *
 * @param just_pressed  Bits that just transitioned pressed this tick.
 *                      Bit N = BIT(AKIRA_BTN_*).
 */
void home_screen_handle_key(uint32_t just_pressed);

#ifdef __cplusplus
}
#endif

#endif /* HOME_SCREEN_H */
