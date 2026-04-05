/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file shell_theme.h
 * @brief AkiraConsole OS Shell — shared color and layout constants.
 *
 * Playdate-inspired monochrome aesthetic rendered via akira_display_* API.
 * All colors are RGB565 values matching the akira_api.h COLOR_* palette.
 */

#ifndef SHELL_THEME_H
#define SHELL_THEME_H

/* ------------------------------------------------------------------ */
/* Screen geometry (320×240 landscape)                                 */
/* ------------------------------------------------------------------ */
#define SHELL_SCREEN_W   320
#define SHELL_SCREEN_H   240
#define SHELL_HEADER_H    32   /* Black top bar (title + nav hints)  */
#define SHELL_FOOTER_H    30   /* Black bottom bar (button hints)    */
#define SHELL_CONTENT_H  (SHELL_SCREEN_H - SHELL_HEADER_H - SHELL_FOOTER_H)

/* ------------------------------------------------------------------ */
/* RGB565 colour palette                                               */
/* ------------------------------------------------------------------ */
#define SHELL_C_BLACK    0x0000u   /* pure black                      */
#define SHELL_C_WHITE    0xFFFFu   /* pure white                      */
#define SHELL_C_GRAY     0x7BEFu   /* mid-gray (COLOR_GRAY)           */
#define SHELL_C_DKGRAY   0x39E7u   /* dark gray (COLOR_DARK_GRAY)     */
#define SHELL_C_LTGRAY   0xD6BAu   /* light gray separator            */
#define SHELL_C_RED      0xF800u   /* error indicator                 */

/* Semantic aliases */
#define SHELL_COLOR_BG           SHELL_C_WHITE
#define SHELL_COLOR_FG           SHELL_C_BLACK
#define SHELL_COLOR_HEADER_BG    SHELL_C_BLACK
#define SHELL_COLOR_HEADER_TXT   SHELL_C_WHITE
#define SHELL_COLOR_FOOTER_BG    SHELL_C_BLACK
#define SHELL_COLOR_FOOTER_TXT   SHELL_C_WHITE
#define SHELL_COLOR_SELECTED_BG  SHELL_C_BLACK
#define SHELL_COLOR_SELECTED_TXT SHELL_C_WHITE
#define SHELL_COLOR_SEPARATOR    SHELL_C_LTGRAY
#define SHELL_COLOR_DOT_RUN      SHELL_C_BLACK
#define SHELL_COLOR_DOT_STOP     SHELL_C_GRAY
#define SHELL_COLOR_DOT_ERR      SHELL_C_RED

#endif /* SHELL_THEME_H */
