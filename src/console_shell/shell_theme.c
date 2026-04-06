/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "shell_theme.h"

#if defined(CONFIG_LVGL)

/* ------------------------------------------------------------------ */
/* Shared LVGL style objects                                           */
/* ------------------------------------------------------------------ */

lv_style_t g_style_screen;
lv_style_t g_style_list_item;
lv_style_t g_style_card;
lv_style_t g_style_separator;

void shell_theme_init(void)
{
    /* Screen: plain white background */
    lv_style_init(&g_style_screen);
    lv_style_set_bg_color(&g_style_screen, lv_color_white());
    lv_style_set_bg_opa(&g_style_screen, LV_OPA_COVER);
    lv_style_set_border_width(&g_style_screen, 0);
    lv_style_set_pad_all(&g_style_screen, 0);

    /* List item: white bg, black text, light bottom border */
    lv_style_init(&g_style_list_item);
    lv_style_set_bg_color(&g_style_list_item, lv_color_white());
    lv_style_set_bg_opa(&g_style_list_item, LV_OPA_COVER);
    lv_style_set_text_color(&g_style_list_item, lv_color_black());
    lv_style_set_text_font(&g_style_list_item, SHELL_FONT_SMALL);
    lv_style_set_border_width(&g_style_list_item, 1);
    lv_style_set_border_color(&g_style_list_item,
                              lv_color_make(0xD6, 0xBA, 0x00));
    lv_style_set_border_side(&g_style_list_item, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_pad_ver(&g_style_list_item, 6);
    lv_style_set_pad_hor(&g_style_list_item, 8);
    lv_style_set_radius(&g_style_list_item, 0);

    /* Card: white bg, thin dark border, rounded corners */
    lv_style_init(&g_style_card);
    lv_style_set_bg_color(&g_style_card, lv_color_white());
    lv_style_set_bg_opa(&g_style_card, LV_OPA_COVER);
    lv_style_set_border_color(&g_style_card, lv_color_black());
    lv_style_set_border_width(&g_style_card, 2);
    lv_style_set_radius(&g_style_card, 6);
    lv_style_set_pad_all(&g_style_card, 8);

    /* Separator: thin horizontal light-gray bar */
    lv_style_init(&g_style_separator);
    lv_style_set_bg_color(&g_style_separator, lv_color_make(0xD6, 0xBA, 0x00));
    lv_style_set_bg_opa(&g_style_separator, LV_OPA_COVER);
    lv_style_set_border_width(&g_style_separator, 0);
    lv_style_set_radius(&g_style_separator, 0);
    lv_style_set_pad_all(&g_style_separator, 0);
}

/* ------------------------------------------------------------------ */
/* Header / footer builders                                            */
/* ------------------------------------------------------------------ */

void shell_theme_make_header(lv_obj_t *parent, const char *title)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SHELL_SCREEN_W, SHELL_HEADER_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, SHELL_FONT_SMALL, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
}

void shell_theme_make_footer(lv_obj_t *parent, const char *left_hint,
                              const char *right_hint)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SHELL_SCREEN_W, SHELL_FOOTER_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    if (left_hint && left_hint[0] != '\0') {
        lv_obj_t *ll = lv_label_create(bar);
        lv_label_set_text(ll, left_hint);
        lv_obj_set_style_text_color(ll, lv_color_white(), 0);
        lv_obj_set_style_text_font(ll, SHELL_FONT_SMALL, 0);
        lv_obj_align(ll, LV_ALIGN_LEFT_MID, 8, 0);
    }

    if (right_hint && right_hint[0] != '\0') {
        lv_obj_t *rl = lv_label_create(bar);
        lv_label_set_text(rl, right_hint);
        lv_obj_set_style_text_color(rl, lv_color_white(), 0);
        lv_obj_set_style_text_font(rl, SHELL_FONT_SMALL, 0);
        lv_obj_align(rl, LV_ALIGN_RIGHT_MID, -8, 0);
    }
}

#endif /* CONFIG_LVGL */

