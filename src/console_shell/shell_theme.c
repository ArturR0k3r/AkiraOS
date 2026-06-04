/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_shell_theme
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_shell_theme, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file shell_theme.c
 * @brief AkiraConsole OS Shell — LVGL theme initialisation.
 *
 * All shared lv_style_t objects are populated here once at boot and then
 * referenced throughout the shell screens.  This avoids redundant style
 * initialisation and ensures visual consistency.
 */

#include "shell_theme.h"

#if defined(CONFIG_LVGL)
#include <lvgl.h>

/* ------------------------------------------------------------------ */
/* Shared style instances                                              */
/* ------------------------------------------------------------------ */

lv_style_t g_style_screen;
lv_style_t g_style_header;
lv_style_t g_style_footer;
lv_style_t g_style_list_item;
lv_style_t g_style_selected;
lv_style_t g_style_card;
lv_style_t g_style_separator;

/* ------------------------------------------------------------------ */

void shell_theme_init(void)
{
    /* ------ Screen (full white canvas) ------ */
    lv_style_init(&g_style_screen);
    lv_style_set_bg_color(&g_style_screen, lv_color_white());
    lv_style_set_bg_opa(&g_style_screen, LV_OPA_COVER);
    lv_style_set_pad_all(&g_style_screen, 0);
    lv_style_set_border_width(&g_style_screen, 0);
    lv_style_set_radius(&g_style_screen, 0);

    /* ------ Header bar (black, white text, Montserrat 20) ------ */
    lv_style_init(&g_style_header);
    lv_style_set_bg_color(&g_style_header, lv_color_black());
    lv_style_set_bg_opa(&g_style_header, LV_OPA_COVER);
    lv_style_set_text_color(&g_style_header, lv_color_white());
    lv_style_set_text_font(&g_style_header, SHELL_FONT_LARGE);
    lv_style_set_pad_hor(&g_style_header, 10);
    lv_style_set_pad_ver(&g_style_header, 0);
    lv_style_set_border_width(&g_style_header, 0);
    lv_style_set_radius(&g_style_header, 0);

    /* ------ Footer bar (black, white text, Montserrat 14) ------ */
    lv_style_init(&g_style_footer);
    lv_style_set_bg_color(&g_style_footer, lv_color_black());
    lv_style_set_bg_opa(&g_style_footer, LV_OPA_COVER);
    lv_style_set_text_color(&g_style_footer, lv_color_white());
    lv_style_set_text_font(&g_style_footer, SHELL_FONT_SMALL);
    lv_style_set_pad_hor(&g_style_footer, 10);
    lv_style_set_pad_ver(&g_style_footer, 0);
    lv_style_set_border_width(&g_style_footer, 0);
    lv_style_set_radius(&g_style_footer, 0);

    /* ------ List item (white bg, black text, Montserrat 20, 44 px tall) ------ */
    lv_style_init(&g_style_list_item);
    lv_style_set_bg_color(&g_style_list_item, lv_color_white());
    lv_style_set_bg_opa(&g_style_list_item, LV_OPA_COVER);
    lv_style_set_text_color(&g_style_list_item, lv_color_black());
    lv_style_set_text_font(&g_style_list_item, SHELL_FONT_LARGE);
    lv_style_set_pad_hor(&g_style_list_item, 12);
    lv_style_set_pad_ver(&g_style_list_item, 10);
    lv_style_set_border_width(&g_style_list_item, 0);
    lv_style_set_border_side(&g_style_list_item, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&g_style_list_item, lv_color_hex(0xD6BAu));
    lv_style_set_border_width(&g_style_list_item, 1);
    lv_style_set_radius(&g_style_list_item, 0);
    lv_style_set_min_height(&g_style_list_item, 44);
    lv_style_set_width(&g_style_list_item, SHELL_SCREEN_W);

    /* ------ Selected/focused row (inverted: black bg, white text) ------ */
    lv_style_init(&g_style_selected);
    lv_style_set_bg_color(&g_style_selected, lv_color_black());
    lv_style_set_bg_opa(&g_style_selected, LV_OPA_COVER);
    lv_style_set_text_color(&g_style_selected, lv_color_white());
    lv_style_set_text_font(&g_style_selected, SHELL_FONT_LARGE);
    lv_style_set_pad_hor(&g_style_selected, 12);
    lv_style_set_pad_ver(&g_style_selected, 10);
    lv_style_set_border_width(&g_style_selected, 0);
    lv_style_set_radius(&g_style_selected, 0);
    lv_style_set_min_height(&g_style_selected, 44);
    lv_style_set_width(&g_style_selected, SHELL_SCREEN_W);

    /* ------ Card (rounded white panel with 1 px border) ------ */
    lv_style_init(&g_style_card);
    lv_style_set_bg_color(&g_style_card, lv_color_white());
    lv_style_set_bg_opa(&g_style_card, LV_OPA_COVER);
    lv_style_set_border_color(&g_style_card, lv_color_hex(0xD6BAu));
    lv_style_set_border_width(&g_style_card, 1);
    lv_style_set_radius(&g_style_card, 6);
    lv_style_set_pad_all(&g_style_card, 8);

    /* ------ Separator (thin horizontal line) ------ */
    lv_style_init(&g_style_separator);
    lv_style_set_bg_color(&g_style_separator, lv_color_hex(0xD6BAu));
    lv_style_set_bg_opa(&g_style_separator, LV_OPA_COVER);
    lv_style_set_height(&g_style_separator, 1);
    lv_style_set_border_width(&g_style_separator, 0);
    lv_style_set_pad_all(&g_style_separator, 0);
    lv_style_set_radius(&g_style_separator, 0);

    /* Apply default theme adjustments */
    lv_theme_t *th = lv_theme_default_init(
        lv_display_get_default(),
        lv_color_black(),       /* primary */
        lv_color_make(50,50,50),/* secondary */
        false,                  /* dark mode = off */
        SHELL_FONT_SMALL
    );
    lv_display_set_theme(lv_display_get_default(), th);

    LOG_INF("Shell theme initialised (Playdate-style, 320×240)");
}

/* ------------------------------------------------------------------ */
/* Helper: header bar                                                  */
/* ------------------------------------------------------------------ */

lv_obj_t *shell_theme_make_header(lv_obj_t *parent, const char *title)
{
    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, SHELL_SCREEN_W, SHELL_HEADER_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_add_style(hdr, &g_style_header, 0);
    lv_obj_set_scrollbar_mode(hdr, LV_SCROLLBAR_MODE_OFF);

    if (title) {
        lv_obj_t *lbl = lv_label_create(hdr);
        lv_obj_add_style(lbl, &g_style_header, 0);
        lv_label_set_text(lbl, title);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
    return hdr;
}

/* ------------------------------------------------------------------ */
/* Helper: footer bar                                                  */
/* ------------------------------------------------------------------ */

lv_obj_t *shell_theme_make_footer(lv_obj_t *parent,
                                  const char *hint_left,
                                  const char *hint_right)
{
    lv_obj_t *ftr = lv_obj_create(parent);
    lv_obj_set_size(ftr, SHELL_SCREEN_W, SHELL_FOOTER_H);
    lv_obj_set_pos(ftr, 0, SHELL_SCREEN_H - SHELL_FOOTER_H);
    lv_obj_add_style(ftr, &g_style_footer, 0);
    lv_obj_set_scrollbar_mode(ftr, LV_SCROLLBAR_MODE_OFF);

    if (hint_left) {
        lv_obj_t *lbl = lv_label_create(ftr);
        lv_obj_add_style(lbl, &g_style_footer, 0);
        lv_label_set_text(lbl, hint_left);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }

    if (hint_right) {
        lv_obj_t *lbl = lv_label_create(ftr);
        lv_obj_add_style(lbl, &g_style_footer, 0);
        lv_label_set_text(lbl, hint_right);
        lv_obj_align(lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return ftr;
}

#endif /* CONFIG_LVGL */
