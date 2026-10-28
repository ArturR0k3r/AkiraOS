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

/* Battery semantic accents — the only hues in the shell.  Everything else
 * stays 1-bit; colour is reserved for the one state worth a glance: power.
 * Picked dark enough to read on the white background. */
#define SHELL_C_CHARGE   0x0549u   /* green  #00A848 — charging / full     */
#define SHELL_C_WARN     0xE480u   /* amber  #E09000 — low battery         */
#define SHELL_C_CRIT     0xF800u   /* red    #FF0000 — critical battery    */

/* Flat short aliases (used by all Liquid Crystal screens) */
#define C_BLACK       SHELL_C_BLACK
#define C_WHITE       SHELL_C_WHITE
#define C_GRAY        SHELL_C_GRAY
#define C_DKGRAY      SHELL_C_DKGRAY

/* ------------------------------------------------------------------ */
/* Screen geometry (landscape — width from CONFIG_AKIRA_OS_SHELL_SCREEN_W) */
/* ------------------------------------------------------------------ */
#define SCR_W    CONFIG_AKIRA_OS_SHELL_SCREEN_W
#define SCR_H    240
#define SBAR_H   24    /* status bar height (akira_ui status pill)    */
#define CONT_Y   26    /* content area start y                        */
#define FOOT_Y   216   /* footer hint bar start y                     */
#define FOOT_H   24    /* footer hint bar height                      */

#endif /* SHELL_THEME_H */
