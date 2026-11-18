/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file home_screen.h
 * @brief AkiraOS home launcher — the root screen of the shell's stack.
 */

#ifndef HOME_SCREEN_H
#define HOME_SCREEN_H

#include "ui/screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The launcher; pass to screen_init() as the root. */
extern const shell_screen_t home_screen;

/** Re-read the installed-app list and repaint (app installed/removed/started). */
void home_screen_refresh(void);

/**
 * Fan the carousel out from behind the focused card — played after the boot
 * splash and whenever the shell takes the display back from an app.
 */
void home_screen_play_intro(void);

#ifdef __cplusplus
}
#endif

#endif /* HOME_SCREEN_H */
