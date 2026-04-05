/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file shell_theme.h
 * @brief AkiraConsole OS Shell — LVGL theme and shared style constants.
 *
 * Visual identity: Playdate-inspired.
 *   - White background, true-black header/footer bars, Montserrat Bold fonts
 *   - Monochrome accent: selected rows invert (black bg, white text)
 *   - Status dots: filled circle = running, outline = stopped, red = error
 *   - Compact layout for 320×240 display
 */

#ifndef SHELL_THEME_H
#define SHELL_THEME_H

#if defined(CONFIG_LVGL)
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Screen geometry                                                     */
/* ------------------------------------------------------------------ */
#define SHELL_SCREEN_W   320
#define SHELL_SCREEN_H   240
#define SHELL_HEADER_H    32   /* Black top bar (title + nav hints) */
#define SHELL_FOOTER_H    24   /* Black bottom bar (button hints)   */
#define SHELL_CONTENT_H  (SHELL_SCREEN_H - SHELL_HEADER_H - SHELL_FOOTER_H)

/* ------------------------------------------------------------------ */
/* Colour palette                                                      */
/* ------------------------------------------------------------------ */
#define SHELL_COLOR_BG          lv_color_white()
#define SHELL_COLOR_ACCENT       lv_color_black()
#define SHELL_COLOR_HEADER_BG    lv_color_black()
#define SHELL_COLOR_HEADER_TXT   lv_color_white()
#define SHELL_COLOR_SELECTED_BG  lv_color_black()
#define SHELL_COLOR_SELECTED_TXT lv_color_white()
#define SHELL_COLOR_TEXT         lv_color_black()
#define SHELL_COLOR_SUBTEXT      lv_color_make(80, 80, 80)
#define SHELL_COLOR_DOT_RUN      lv_color_black()
#define SHELL_COLOR_DOT_STOP     lv_color_make(180, 180, 180)
#define SHELL_COLOR_DOT_ERR      lv_color_make(220, 50,  50)
#define SHELL_COLOR_SEPARATOR    lv_color_make(210, 210, 210)

/* ------------------------------------------------------------------ */
/* Fonts                                                               */
/* ------------------------------------------------------------------ */
#define SHELL_FONT_SMALL   (&lv_font_montserrat_14)
#define SHELL_FONT_LARGE   (&lv_font_montserrat_20)
#define SHELL_FONT_DEFAULT  SHELL_FONT_SMALL

/* ------------------------------------------------------------------ */
/* Pre-built styles (available after akira_shell_theme_init())         */
/* ------------------------------------------------------------------ */
extern lv_style_t g_style_screen;      /* Full-screen white bg      */
extern lv_style_t g_style_header;      /* Black bar, white text      */
extern lv_style_t g_style_footer;      /* Black bar, white text      */
extern lv_style_t g_style_list_item;   /* Row: white bg, black text  */
extern lv_style_t g_style_selected;    /* Row: black bg, white text  */
extern lv_style_t g_style_card;        /* Rounded white card         */
extern lv_style_t g_style_separator;   /* 1 px horizontal divider    */

/**
 * @brief Initialise the OS shell LVGL theme and populate shared styles.
 *
 * Must be called once after lv_init() and before any screen is created.
 */
void akira_shell_theme_init(void);

/**
 * @brief Apply the header style and set a title label on a parent object.
 *
 * @param parent  Parent widget (usually the screen).
 * @param title   Title string displayed in the header bar.
 * @return The created header container widget.
 */
lv_obj_t *shell_theme_make_header(lv_obj_t *parent, const char *title);

/**
 * @brief Apply the footer style and set button-hint labels.
 *
 * @param parent    Parent widget (usually the screen).
 * @param hint_left  Label for B (left hint), e.g. "B:Back".
 * @param hint_right Label for A (right hint), e.g. "A:Select".
 * @return The created footer container widget.
 */
lv_obj_t *shell_theme_make_footer(lv_obj_t *parent,
                                  const char *hint_left,
                                  const char *hint_right);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_LVGL */
#endif /* SHELL_THEME_H */
