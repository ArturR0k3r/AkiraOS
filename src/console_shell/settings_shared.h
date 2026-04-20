/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * settings_shared.h — Shared Liquid Glass UI primitives for settings sub-screens.
 *
 * Implementations live in settings_screen.c (non-static wrappers).
 * Sub-screens include this header as "../settings_shared.h".
 */
#ifndef SETTINGS_SHARED_H
#define SETTINGS_SHARED_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Layout constants (must match settings_screen.c geometry)           */
/* ------------------------------------------------------------------ */
#define SS_SCR_W    320
#define SS_SCR_H    240
#define SS_SBAR_H   24         /* status/header bar height             */
#define SS_CONT_Y   26         /* content area start y                 */
#define SS_RIB_Y    216        /* ribbon/footer start y                */
#define SS_RIB_H    24         /* ribbon height                        */
#define SS_MENU_X   16         /* menu item left x                     */
#define SS_MENU_W   288        /* menu item width  (SCR_W - 32)        */
#define SS_MENU_ITH 32         /* menu item height                     */

/* ------------------------------------------------------------------ */
/* RGB565 palette (matching settings_screen.c defines)                */
/* ------------------------------------------------------------------ */
#define SS_C_BLACK      0x0000u
#define SS_C_WHITE      0xFFFFu
#define SS_C_GRAY       0x7BEFu
#define SS_C_DKGRAY     0x39E7u
#define SS_C_GLASS_BODY 0x0000u

/* ------------------------------------------------------------------ */
/* Shared draw functions — implemented in settings_screen.c           */
/* ------------------------------------------------------------------ */

/** Focused (selected) glass button. */
void ss_glass_rect_focus(int x, int y, int w, int h, int r);

/** Dimmed (unselected) glass button. */
void ss_glass_rect_dim(int x, int y, int w, int h, int r);

/** Horizontally centred text in a box. bg fills the bounding rect. */
void ss_draw_centred(int x, int y, int w, const char *s,
                     uint16_t fg, uint16_t bg);

/** Header bar with centred title. */
void ss_draw_header(const char *title);

/** Footer ribbon with left and right hint text. */
void ss_draw_ribbon(const char *left, const char *right);

/** Number of SS_MENU_ITH items visible between top_y and the ribbon. */
int ss_menu_vis_count(int top_y);

/**
 * Clamp scroll so sel is always visible.
 * Returns the new scroll offset.
 */
int ss_scroll_clamp(int sel, int scroll, int count, int top_y);

/**
 * Render a scrollable glass-button list.
 * Each item shows the label centred with a ">" indicator.
 * @param labels  NULL-terminated array of label strings
 * @param count   total number of items
 * @param sel     currently selected index
 * @param top_y   y coordinate of the first item row
 * @param scroll  index of the first visible item
 */
void ss_draw_menu_at(const char **labels, int count, int sel,
                     int top_y, int scroll);

#endif /* SETTINGS_SHARED_H */
