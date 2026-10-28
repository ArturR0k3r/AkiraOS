/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file akiraplay_screen.h
 * @brief AkiraPlay — on-device app store browsing the AkiraConsoleApp catalog.
 */

#ifndef AKIRAPLAY_SCREEN_H
#define AKIRAPLAY_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Push AkiraPlay onto the shell's screen stack (from the home launcher).
 * Fetches the catalogue over WiFi once its loading frame is up; B (or HOME
 * long-press) pops back to whatever was underneath.
 */
void akiraplay_open(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRAPLAY_SCREEN_H */
