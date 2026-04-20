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

#if defined(CONFIG_LVGL)
#include <lvgl.h>
#endif

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

/* Flat short aliases (used by all Liquid Crystal screens) */
#define C_BLACK       SHELL_C_BLACK
#define C_WHITE       SHELL_C_WHITE
#define C_GRAY        SHELL_C_GRAY
#define C_DKGRAY      SHELL_C_DKGRAY
#define C_GLASS       0x2104u   /* dither pixel (unused — no dithering) */
#define C_GLASS_BODY  0x0000u   /* glass fill = black (transparent look) */
#define C_GLASS_HILIT 0x7BEFu   /* glass top-sheen highlight          */

/* ------------------------------------------------------------------ */
/* Screen geometry (320×240 landscape, shared across all screens)     */
/* ------------------------------------------------------------------ */
#define SCR_W    320
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

#if defined(CONFIG_LVGL)

/** Small text font (Montserrat 14) */
#define SHELL_FONT_SMALL  (&lv_font_montserrat_14)

/** LVGL-typed colors for use with lv_obj_set_style_* API */
#define SHELL_LVGL_COLOR_BG      lv_color_white()
#define SHELL_LVGL_COLOR_FG      lv_color_black()
#define SHELL_LVGL_COLOR_SUBTEXT lv_color_make(0x80, 0x80, 0x80)
#define SHELL_COLOR_SUBTEXT      SHELL_LVGL_COLOR_SUBTEXT

/** White-background screen style */
extern lv_style_t g_style_screen;

/** List-item row style (white bg, dark text, padding) */
extern lv_style_t g_style_list_item;

/** Dialog card style (white bg, thin border) */
extern lv_style_t g_style_card;

/** Horizontal separator line style */
extern lv_style_t g_style_separator;

/**
 * @brief Initialise shared shell LVGL styles.  Call once before any
 *        settings sub-screen is created.
 */
void shell_theme_init(void);

/**
 * @brief Attach a black title bar to @p parent.
 * @param parent  Screen object.
 * @param title   Text to display in the header.
 */
void shell_theme_make_header(lv_obj_t *parent, const char *title);

/**
 * @brief Attach a black hint bar to @p parent.
 * @param parent      Screen object.
 * @param left_hint   Left-side button hint (e.g. "B:Back").
 * @param right_hint  Right-side button hint (e.g. "A:OK").
 */
void shell_theme_make_footer(lv_obj_t *parent, const char *left_hint,
                              const char *right_hint);

#endif /* CONFIG_LVGL */

#endif /* SHELL_THEME_H */
