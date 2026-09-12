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

/** Enter AkiraPlay from the home launcher. Fetches the catalogue over WiFi. */
void akiraplay_screen_load(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRAPLAY_SCREEN_H */
