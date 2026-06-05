/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_shell_theme
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_shell_theme, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file shell_theme.c
 * @brief AkiraConsole OS Shell — theme initialisation (LVGL removed).
 */

#include "shell_theme.h"

void shell_theme_init(void)
{
    LOG_DBG("Shell theme init (LVGL removed)");
}
