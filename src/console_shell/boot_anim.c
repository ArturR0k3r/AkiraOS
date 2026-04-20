/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file boot_anim.c
 * @brief AkiraOS boot logo animation – single-sprite, 60-frame, ≤ 2 s total.
 *
 * Uses one 116×116 RGB565 source sprite (boot_logo_sprite.h) and scales it
 * at runtime using nearest-neighbour interpolation.  The output is written
 * one scanline at a time into a 116-pixel row buffer (232 B) so no large
 * frame buffer is needed in DRAM.
 *
 * Animation sequence
 * ──────────────────
 * Phase 1 (grow,  40 frames × 2 ms):  star scales 20 px → 120 px, ease-out.
 * Phase 2 (combo, 25 frames × 4 ms):  star slides left & "AKIRA CONSOLE"
 *                                       slides in simultaneously, ease-out.
 * Phase 3 (tag,   15 frames × 4 ms):  "RUNNING ON AKIRAOS" slides up
 *                                       into place below "CONSOLE".
 * Hold 200 ms.
 *
 * Total ≈ 80 + 100 + 60 + 200 = 440 ms.
 */

#define LOG_MODULE_NAME boot_anim
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(boot_anim, CONFIG_AKIRA_LOG_LEVEL);

#include "boot_anim.h"
#include "boot_logo_sprite.h"
#include <api/akira_display_api.h>
#include <zephyr/kernel.h>

/* ── Display defaults ──────────────────────────────────────────────────── */
#define BA_DEFAULT_W  240
#define BA_DEFAULT_H  320

/* ── Colors ─────────────────────────────────────────────────────────────── */
#define BA_BG_COLOR    ((uint16_t)CONFIG_AKIRA_BOOT_ANIM_BG_COLOR)
#define BA_TEXT_COLOR  ((uint16_t)CONFIG_AKIRA_BOOT_ANIM_TEXT_COLOR)

/* ── Phase timing (ms / frame) ─────────────────────────────────────────── */
#define T_GROW     CONFIG_AKIRA_BOOT_ANIM_GROW_FRAME_MS
#define T_COMBO    CONFIG_AKIRA_BOOT_ANIM_SLIDE_FRAME_MS

/* ── Grow boundary ──────────────────────────────────────────────────────── */
#define BA_START_SIZE  20    /* smallest size in grow phase */
#define BA_REST_SIZE   120   /* final size (used by combo phase too) */

/* ── Row scratch buffer: one scanline of the scaled sprite (232 B) ────── */
static uint16_t s_row_buf[LOGO_SPRITE_SIZE];

/* ─────────────────────────────────────────────────────────────────────────
 * ease_out: cubic f(t)=1-(1-t)^3 over 'total' frames → [0,255]
 * Steeper than quadratic: snaps quickly then decelerates sharply.
 * ───────────────────────────────────────────────────────────────────────── */
static int ease_out(int frame, int total)
{
    if (total <= 1) {
        return 255;
    }
    int t = frame * 256 / (total - 1);
    if (t > 256) {
        t = 256;
    }
    int inv = 256 - t;
    int v = 256 - (inv * inv / 256) * inv / 256;
    return (v > 255) ? 255 : v;
}

/* ─────────────────────────────────────────────────────────────────────────
 * draw_sprite_scaled
 *
 * Nearest-neighbour scale g_star_sprite (LOGO_SPRITE_SIZE × LOGO_SPRITE_SIZE)
 * into a dst_size × dst_size region centred at pixel (cx, cy).
 *
 * Rendered one scanline at a time using s_row_buf (232 B), so no large
 * frame-buffer is needed in DRAM.
 * ───────────────────────────────────────────────────────────────────────── */
static void draw_sprite_scaled(int cx, int cy, int dst_size)
{
    if (dst_size < 1) {
        dst_size = 1;
    }
    if (dst_size > LOGO_SPRITE_SIZE) {
        dst_size = LOGO_SPRITE_SIZE;
    }

    const int src = LOGO_SPRITE_SIZE;
    int x0 = cx - dst_size / 2;
    int y0 = cy - dst_size / 2;

    for (int oy = 0; oy < dst_size; oy++) {
        int sy = (dst_size > 1) ? oy * (src - 1) / (dst_size - 1) : 0;
        const uint16_t *src_row = &g_star_sprite[sy * src];

        for (int ox = 0; ox < dst_size; ox++) {
            int sx = (dst_size > 1) ? ox * (src - 1) / (dst_size - 1) : 0;
            s_row_buf[ox] = src_row[sx];
        }

        /* Blit one scanline — transparent pixels are skipped by the driver */
        akira_display_bitmap_transparent(x0, y0 + oy, dst_size, 1,
                                         s_row_buf, LOGO_TRANSPARENT_KEY);
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Phase 1: grow — ease-out scale from BA_START_SIZE → BA_REST_SIZE
 * ───────────────────────────────────────────────────────────────────────── */
#define GROW_FRAMES   40

static void phase_grow(int cx, int cy)
{
    int range = BA_REST_SIZE - BA_START_SIZE;
    for (int i = 0; i < GROW_FRAMES; i++) {
        int sz = BA_START_SIZE + (range * ease_out(i, GROW_FRAMES)) / 255;
        akira_display_clear(BA_BG_COLOR);
        draw_sprite_scaled(cx, cy, sz);
        akira_display_flush();
        k_sleep(K_MSEC(T_GROW));
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Phase 3: combined — logo slides left while "AKIRA / CONSOLE" slides in
 *          from the right, both using the same ease-out curve so they
 *          feel like one fluid motion.
 * ───────────────────────────────────────────────────────────────────────── */
#define COMBO_FRAMES   25

static void phase_slide_and_text(int cx_start, int cx_end, int cy, int disp_w)
{
    int logo_dist    = cx_end - cx_start;          /* negative: left */
    int text_x_end   = cx_end + BA_REST_SIZE / 2 + 6;
    int text_x_start = disp_w + 4;                 /* start off-screen right */
    int text_dist    = text_x_end - text_x_start;  /* negative: slides left */
    /* Two-line layout: "AKIRA" above centre, "CONSOLE" below */
    int line1_y = cy - 12;
    int line2_y = cy + 4;

    for (int i = 0; i < COMBO_FRAMES; i++) {
        int e  = ease_out(i, COMBO_FRAMES);
        int cx = cx_start + (logo_dist * e) / 255;
        int tx = text_x_start + (text_dist * e) / 255;
        akira_display_clear(BA_BG_COLOR);
        draw_sprite_scaled(cx, cy, BA_REST_SIZE);
        akira_display_text_large(tx, line1_y, "AKIRA",   BA_TEXT_COLOR);
        akira_display_text_large(tx, line2_y, "CONSOLE", BA_TEXT_COLOR);
        akira_display_flush();
        k_sleep(K_MSEC(T_COMBO));
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Phase 3: tagline — "RUNNING ON AKIRAOS" slides up from below while
 *          the logo and "AKIRA CONSOLE" stay static.
 * ───────────────────────────────────────────────────────────────────────── */
#define TAGLINE_FRAMES  15
#define T_TAGLINE        CONFIG_AKIRA_BOOT_ANIM_TAG_FRAME_MS

static void phase_tagline(int cx_final, int cy, int disp_w, int disp_h)
{
    int text_x   = cx_final + BA_REST_SIZE / 2 + 6;
    int line1_y  = cy - 12;
    int line2_y  = cy + 4;
    /* tagline slides up from off-screen bottom */
    int tag_y_end   = cy + 22;
    int tag_y_start = disp_h + 8;
    int tag_dist    = tag_y_end - tag_y_start;

    for (int i = 0; i < TAGLINE_FRAMES; i++) {
        int e  = ease_out(i, TAGLINE_FRAMES);
        int ty = tag_y_start + (tag_dist * e) / 255;
        akira_display_clear(BA_BG_COLOR);
        draw_sprite_scaled(cx_final, cy, BA_REST_SIZE);
        akira_display_text_large(text_x, line1_y, "AKIRA",            BA_TEXT_COLOR);
        akira_display_text_large(text_x, line2_y, "CONSOLE",          BA_TEXT_COLOR);
        akira_display_text(text_x,       ty,      "RUNNING ON AKIRAOS", BA_TEXT_COLOR);
        akira_display_flush();
        k_sleep(K_MSEC(T_TAGLINE));
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * boot_anim_run – public entry point
 * ───────────────────────────────────────────────────────────────────────── */
void boot_anim_run(void)
{
    int disp_w = BA_DEFAULT_W;
    int disp_h = BA_DEFAULT_H;
    akira_display_get_size(&disp_w, &disp_h);
    if (disp_w <= 0) disp_w = BA_DEFAULT_W;
    if (disp_h <= 0) disp_h = BA_DEFAULT_H;

    int cx_centre = disp_w / 2;
    int cy        = disp_h / 2;
    int cx_final  = disp_w / 4;

    LOG_DBG("boot_anim: %dx%d  star settles x=%d", disp_w, disp_h, cx_final);

    akira_display_clear(BA_BG_COLOR);
    akira_display_flush();

    phase_grow(cx_centre, cy);
    phase_slide_and_text(cx_centre, cx_final, cy, disp_w);
    phase_tagline(cx_final, cy, disp_w, disp_h);

    k_sleep(K_MSEC(CONFIG_AKIRA_BOOT_DELAY_MS));
}
