/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file display_screen.h
 * @brief Brightness and screen-off timeout settings.
 */

#ifndef SETTINGS_DISPLAY_SCREEN_H
#define SETTINGS_DISPLAY_SCREEN_H

#include "ui/list_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Settings → Display (push with list_screen_open()). */
extern const list_model_t display_page;

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_DISPLAY_SCREEN_H */
