/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file datetime_screen.h
 * @brief Date and time settings (SNTP sync and manual entry).
 */

#ifndef SETTINGS_DATETIME_SCREEN_H
#define SETTINGS_DATETIME_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/** Settings → Date & time: push the screen on the shell's stack. */
void datetime_open(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_DATETIME_SCREEN_H */
