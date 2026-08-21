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
/* Screen geometry (landscape — width set by CONFIG_AKIRA_OS_SHELL_SCREEN_W) */
/* ------------------------------------------------------------------ */
#define SHELL_SCREEN_W   CONFIG_AKIRA_OS_SHELL_SCREEN_W
#define SHELL_SCREEN_H   240
#define SHELL_HEADER_H    32   /* Black top bar (title + nav hints)  */
#define SHELL_FOOTER_H    30   /* Black bottom bar (button hints)    */
#define SHELL_CONTENT_H  (SHELL_SCREEN_H - SHELL_HEADER_H - SHELL_FOOTER_H)

/* ------------------------------------------------------------------ */
/* RGB565 colour palette                                               */
/* ------------------------------------------------------------------ */
/* THEME: light (black on white).
 *
 * Every screen clears to SHELL_C_BLACK and draws in SHELL_C_WHITE, so these
 * two names denote ROLES — background and foreground — not literal colours.
 * To go back to the dark theme, swap the two values below and restore the
 * grays to their commented originals; no call site changes.
 *
 * The grays are the exact per-component RGB565 inverses of the dark-theme
 * values, so every contrast relationship is preserved: what used to sit just
 * above the background (DKGRAY on black) still sits just below it on white. */
#define SHELL_C_BLACK    0xFFFFu   /* BACKGROUND — white (was 0x0000) */
#define SHELL_C_WHITE    0x0000u   /* FOREGROUND — black (was 0xFFFF) */
#define SHELL_C_GRAY     0x8410u   /* mid-gray, de-emphasised (was 0x7BEF) */
#define SHELL_C_DKGRAY   0xC618u   /* nearest the background (was 0x39E7)  */
#define SHELL_C_LTGRAY   0x2945u   /* separator, high contrast (was 0xD6BA) */
#define SHELL_C_RED      0xF800u   /* error indicator — unchanged     */

/* Flat short aliases (used by all Liquid Crystal screens) */
#define C_BLACK       SHELL_C_BLACK
#define C_WHITE       SHELL_C_WHITE
#define C_GRAY        SHELL_C_GRAY
#define C_DKGRAY      SHELL_C_DKGRAY
#define C_GLASS       0xDEFBu   /* dither pixel (unused — no dithering) */
#define C_GLASS_BODY  0xFFFFu   /* glass fill = background (transparent) */
#define C_GLASS_HILIT 0x8410u   /* glass top-sheen highlight          */

/* ------------------------------------------------------------------ */
/* Screen geometry (landscape — width from CONFIG_AKIRA_OS_SHELL_SCREEN_W) */
/* ------------------------------------------------------------------ */
#define SCR_W    CONFIG_AKIRA_OS_SHELL_SCREEN_W
#define SCR_H    240
#define SBAR_H   24    /* status / header bar height                  */
#define LIST_Y   26    /* content list start y                        */
#define FOOT_Y   216   /* footer bar start y                          */
#define FOOT_H   24    /* footer bar height                           */
#define ITEM_H   36    /* settings list item height                   */
#define ITEM_X   12    /* settings list item left margin              */
#define ITEM_W   (SCR_W - 24)  /* settings list item width            */

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

/* ------------------------------------------------------------------ */
/* LVGL styles (shared across all settings sub-screens)                */
/* ------------------------------------------------------------------ */

#endif /* SHELL_THEME_H */
