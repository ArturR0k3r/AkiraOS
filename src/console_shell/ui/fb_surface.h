/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fb_surface.h
 * @brief Direct framebuffer access for the shell's per-pixel renderers
 *        (logo, Akira, effects): one capability lookup per draw call instead
 *        of per pixel, RGB565 blending, ordered dithering.
 *
 * On native_sim the simulated framebuffer is used instead.  Header-only.
 */

#ifndef AKIRA_SHELL_FB_SURFACE_H
#define AKIRA_SHELL_FB_SURFACE_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#include <api/akira_display_api.h>
#include <drivers/platform_hal.h>

#if !AKIRA_PLATFORM_NATIVE_SIM && defined(CONFIG_DISPLAY)
#include <zephyr/drivers/display.h>
#endif

typedef struct {
    uint16_t *fb; /* NULL on native_sim */
    int w, h;
    bool mono;    /* 1-bit panel: every non-black pixel ends up white */
} surf_t;

static inline bool surf_open(surf_t *s)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    s->fb = NULL;
    s->mono = false;
    akira_display_get_size(&s->w, &s->h);
    return s->w > 0 && s->h > 0;
#elif defined(CONFIG_DISPLAY)
    struct display_capabilities caps;

    if (akira_display_hal_get_capabilities(&caps) < 0) {
        return false;
    }
    s->fb = akira_framebuffer_get();
    s->w = caps.x_resolution;
    s->h = caps.y_resolution;
    s->mono = caps.current_pixel_format == PIXEL_FORMAT_MONO01 ||
              caps.current_pixel_format == PIXEL_FORMAT_MONO10;
    return s->fb != NULL;
#else
    ARG_UNUSED(s);
    return false;
#endif
}

static inline uint16_t px_get(const surf_t *s, int x, int y)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    ARG_UNUSED(s);
    return akira_sim_read_pixel(x, y);
#else
    return s->fb[y * s->w + x];
#endif
}

static inline void px_put(const surf_t *s, int x, int y, uint16_t c)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    ARG_UNUSED(s);
    akira_sim_draw_pixel(x, y, c);
#else
    s->fb[y * s->w + x] = c;
#endif
}

/** 4x4 Bayer matrix, 0..15. */
static const uint8_t k_surf_bayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

/** Bayer threshold 8..248 for (x, y). */
static inline int surf_bayer(int x, int y)
{
    return k_surf_bayer[y & 3][x & 3] * 16 + 8;
}

/** RGB565 linear mix: @p t 0 = @p a, 256 = @p b. */
static inline uint16_t surf_mix(uint16_t a, uint16_t b, int t)
{
    if (t <= 0) {
        return a;
    }
    if (t >= 256) {
        return b;
    }
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;

    return (uint16_t)(((ar + (((br - ar) * t) >> 8)) << 11) |
                      ((ag + (((bg - ag) * t) >> 8)) << 5) | (ab + (((bb - ab) * t) >> 8)));
}

/** Mix with an ordered-dither threshold on the fraction: no gradient bands. */
static inline uint16_t surf_mix_dither(uint16_t a, uint16_t b, int t, int x, int y)
{
    int d = surf_bayer(x, y);
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    int r = ((ar << 8) + (br - ar) * t + d) >> 8;
    int g = ((ag << 8) + (bg - ag) * t + d) >> 8;
    int bl = ((ab << 8) + (bb - ab) * t + d) >> 8;

    return (uint16_t)((CLAMP(r, 0, 31) << 11) | (CLAMP(g, 0, 63) << 5) | CLAMP(bl, 0, 31));
}

/** Luma 0..255 of an RGB565 colour. */
static inline int surf_luma(uint16_t c)
{
    int r = (c >> 11) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;

    return (r * 77 + g * 150 + b * 29) >> 8;
}

/** Blend @p c over the frame at (x, y) with coverage @p a (0..256). */
static inline void surf_blend(const surf_t *s, int x, int y, uint16_t c, int a)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h || a <= 0) {
        return;
    }
    if (s->mono) {
        if (a >= 128) {
            px_put(s, x, y, surf_luma(c) > surf_bayer(x, y) ? 0xFFFFu : 0x0000u);
        }
        return;
    }
    px_put(s, x, y, a >= 256 ? c : surf_mix(px_get(s, x, y), c, a));
}

#endif /* AKIRA_SHELL_FB_SURFACE_H */
