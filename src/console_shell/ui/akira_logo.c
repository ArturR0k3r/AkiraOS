/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_logo.c
 * @brief AkiraOS logo renderer and splash light effects (see akira_logo.h).
 *
 * Writes go straight into the framebuffer (one capability lookup per call,
 * not per pixel): the large logo is ~13k pixels and the boot splash redraws
 * it every frame.  On native_sim the simulated framebuffer is used instead.
 */

#include "akira_logo.h"
#include "fb_surface.h"

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

/* ---- generated data (akira_logo_data.c) ------------------------------- */

extern const uint16_t akira_logo_l_w, akira_logo_l_h;
extern const uint16_t akira_logo_l_rgb[];
extern const uint8_t akira_logo_l_alpha[];
extern const uint8_t akira_logo_l_order[];
extern const uint16_t akira_logo_s_w, akira_logo_s_h;
extern const uint16_t akira_logo_s_rgb[];
extern const uint8_t akira_logo_s_alpha[];

typedef struct {
    const uint16_t *w, *h;
    const uint16_t *rgb;
    const uint8_t *alpha;
    const uint8_t *order; /* NULL: no growth map */
} logo_t;

static const logo_t k_logos[] = {
    [AKIRA_LOGO_LARGE] = { &akira_logo_l_w, &akira_logo_l_h, akira_logo_l_rgb,
                           akira_logo_l_alpha, akira_logo_l_order },
    [AKIRA_LOGO_SMALL] = { &akira_logo_s_w, &akira_logo_s_h, akira_logo_s_rgb,
                           akira_logo_s_alpha, NULL },
};

/* ---- colour --------------------------------------------------------------- */

bool akira_logo_panel_is_mono(void)
{
    surf_t s;

    return surf_open(&s) && s.mono;
}

uint16_t akira_logo_mix(uint16_t a, uint16_t b, int t)
{
    return surf_mix(a, b, t);
}

#define mix_dither surf_mix_dither
#define luma surf_luma
#define k_bayer k_surf_bayer

/* ---- logo --------------------------------------------------------------- */

#define WHITE 0xFFFFu
#define SHINE_W 16   /* half-width of the glint band, logo px */
#define SHINE_MAX 200 /* peak whitening of the glint, /256 */
#define EDGE_MAX 230  /* peak whitening of the growth front, /256 */

int akira_logo_w(akira_logo_size_t size)
{
    return *k_logos[size].w;
}

int akira_logo_h(akira_logo_size_t size)
{
    return *k_logos[size].h;
}

void akira_logo_draw(akira_logo_size_t size, int cx, int cy, const akira_logo_fx_t *fx)
{
    const logo_t *L = &k_logos[size];
    const akira_logo_fx_t def = AKIRA_LOGO_FX_STATIC;
    surf_t s;

    if (!fx) {
        fx = &def;
    }
    if (fx->opacity <= 0 || fx->scale <= 0 || !surf_open(&s)) {
        return;
    }

    const int lw = *L->w, lh = *L->h;
    const int dw = MAX(lw * fx->scale / 256, 1), dh = MAX(lh * fx->scale / 256, 1);
    const int x0 = cx - dw / 2, y0 = cy - dh / 2;
    const int inv = (256 * 256) / fx->scale;           /* source step, Q8 */
    const bool growing = L->order && fx->reveal < AKIRA_LOGO_REVEAL_END;
    const bool shining = fx->shine != AKIRA_LOGO_NO_SHINE;
    const int opacity = MIN(fx->opacity, 256);

    for (int dy = 0; dy < dh; dy++) {
        const int y = y0 + dy;

        if (y < 0 || y >= s.h) {
            continue;
        }
        const int sy = MIN(((2 * dy + 1) * inv) >> 9, lh - 1);

        for (int dx = 0; dx < dw; dx++) {
            const int x = x0 + dx;

            if (x < 0 || x >= s.w) {
                continue;
            }
            const int sx = MIN(((2 * dx + 1) * inv) >> 9, lw - 1);
            const int idx = sy * lw + sx;
            const int a4 = (L->alpha[idx >> 1] >> ((idx & 1) ? 0 : 4)) & 0xF;

            if (!a4) {
                continue;
            }
            uint16_t c = L->rgb[idx];
            int heat = fx->flash;

            if (growing) {
                const int lead = fx->reveal - L->order[idx];

                if (lead < 0) {
                    continue; /* not grown yet */
                }
                if (lead < AKIRA_LOGO_EDGE) {
                    heat = (AKIRA_LOGO_EDGE - lead) * EDGE_MAX / AKIRA_LOGO_EDGE;
                }
            }
            if (shining) {
                const int d = abs(sx + sy - fx->shine);

                if (d < SHINE_W) {
                    heat = MAX(heat, (SHINE_W - d) * SHINE_MAX / SHINE_W);
                }
            }
            if (heat) {
                c = akira_logo_mix(c, WHITE, heat);
            }

            int a = a4 * 17; /* 0..255 */

            a = ((a + (a >> 7)) * opacity) >> 8; /* 0..256 */

            if (s.mono) {
                /* Ink where the blended pixel is darker than the Bayer
                 * threshold; paper elsewhere. */
                uint16_t under = px_get(&s, x, y);
                int l = luma(akira_logo_mix(under, c, a));

                px_put(&s, x, y, l > k_bayer[y & 3][x & 3] * 16 + 8 ? WHITE : 0x0000u);
            } else if (a >= 256) {
                px_put(&s, x, y, c);
            } else {
                px_put(&s, x, y, akira_logo_mix(px_get(&s, x, y), c, a));
            }
        }
    }
}

void akira_logo_draw_at(akira_logo_size_t size, int x, int y)
{
    akira_logo_draw(size, x + akira_logo_w(size) / 2, y + akira_logo_h(size) / 2, NULL);
}

/* ---- light effects ------------------------------------------------------ */

void akira_logo_glow(int cx, int cy, int r, uint16_t colour, int strength)
{
    surf_t s;

    if (strength <= 0 || r <= 0 || !surf_open(&s) || s.mono) {
        return;
    }
    strength = MIN(strength, 256);

    const int r2 = r * r;
    const int ya = MAX(cy - r, 0), yb = MIN(cy + r, s.h - 1);
    const int xa = MAX(cx - r, 0), xb = MIN(cx + r, s.w - 1);

    for (int y = ya; y <= yb; y++) {
        const int dy2 = (y - cy) * (y - cy);

        for (int x = xa; x <= xb; x++) {
            const int d2 = dy2 + (x - cx) * (x - cx);

            if (d2 >= r2) {
                continue;
            }
            /* (1 - d²/r²)² — a soft, round falloff with no visible rim. */
            int f = 256 - (int)((int64_t)d2 * 256 / r2);

            f = (f * f) >> 8;
            px_put(&s, x, y, mix_dither(px_get(&s, x, y), colour, (f * strength) >> 8, x, y));
        }
    }
}

void akira_logo_burst(int cx, int cy, int r, int soft, uint16_t colour)
{
    surf_t s;

    if (r <= 0 || !surf_open(&s)) {
        return;
    }
    soft = MAX(soft, 1);

    const int ro = r + soft;
    const int r2 = r * r, ro2 = ro * ro;
    const int ya = MAX(cy - ro, 0), yb = MIN(cy + ro, s.h - 1);
    const int xa = MAX(cx - ro, 0), xb = MIN(cx + ro, s.w - 1);

    for (int y = ya; y <= yb; y++) {
        const int dy2 = (y - cy) * (y - cy);

        for (int x = xa; x <= xb; x++) {
            const int d2 = dy2 + (x - cx) * (x - cx);

            if (d2 >= ro2) {
                continue;
            }
            if (d2 <= r2 || s.mono) {
                if (d2 <= r2) {
                    px_put(&s, x, y, colour);
                }
                continue;
            }
            /* Across a thin rim d² is near-linear in d: no sqrt needed. */
            int t = 256 - (int)((int64_t)(d2 - r2) * 256 / (ro2 - r2));

            px_put(&s, x, y, mix_dither(px_get(&s, x, y), colour, t, x, y));
        }
    }
}

#define blend_px surf_blend

void akira_logo_sparkle(int cx, int cy, int size, uint16_t colour)
{
    surf_t s;

    if (size <= 0 || !surf_open(&s)) {
        return;
    }
    blend_px(&s, cx, cy, colour, 256);
    /* Long tapering arms on the axes... */
    for (int i = 1; i <= size; i++) {
        int a = (size + 1 - i) * 256 / (size + 1);

        blend_px(&s, cx + i, cy, colour, a);
        blend_px(&s, cx - i, cy, colour, a);
        blend_px(&s, cx, cy + i, colour, a);
        blend_px(&s, cx, cy - i, colour, a);
    }
    /* ...a soft core, and short faint diagonals. */
    for (int i = 1; i <= size / 3; i++) {
        int a = 140 * (size / 3 + 1 - i) / (size / 3 + 1);

        blend_px(&s, cx + i, cy + i, colour, a);
        blend_px(&s, cx - i, cy - i, colour, a);
        blend_px(&s, cx + i, cy - i, colour, a);
        blend_px(&s, cx - i, cy + i, colour, a);
    }
}
