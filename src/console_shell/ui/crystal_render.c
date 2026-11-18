/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file crystal_render.c
 * @brief Procedural Akira crystal + sigils (see crystal_render.h).
 *
 * Fixed point throughout: vertices are Q4 screen coordinates, angles are
 * binary units (65536 per turn), trig comes from a Q14 quarter-wave table,
 * and the rasterizer steps per-edge signed distances in Q16 pixels.
 */

#include "crystal_render.h"
#include "fb_surface.h"

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <lib/mem_helper.h>

#define WHITE 0xFFFFu
#define BLACK 0x0000u
#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

const uint32_t crystal_icon[16] = {
    0x00400000u, 0x00E00000u, 0x20E08000u, 0x19F30000u, 0x0FFE0000u, 0x075C0000u,
    0xFF5FE000u, 0x075C0000u, 0x0FFE0000u, 0x19F30000u, 0x20E08000u, 0x00E00000u,
    0x00400000u, 0x00000000u, 0x00000000u, 0x00000000u,
};

/* ---- trig --------------------------------------------------------------- */

static const int16_t k_qsin[65] = {
    0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981, 4370, 4756,
    5139, 5520, 5897, 6270, 6639, 7005, 7366, 7723, 8076, 8423, 8765, 9102,
    9434, 9760, 10080, 10394, 10702, 11003, 11297, 11585, 11866, 12140, 12406,
    12665, 12916, 13160, 13395, 13623, 13842, 14053, 14256, 14449, 14635, 14811,
    14978, 15137, 15286, 15426, 15557, 15679, 15791, 15893, 15986, 16069, 16143,
    16207, 16261, 16305, 16340, 16364, 16379, 16384,
};

static int qsin(int i) /* 0..16384 → 0..16384 */
{
    int idx = i >> 8, f = i & 255;

    if (idx >= 64) {
        return 16384;
    }
    return k_qsin[idx] + (((k_qsin[idx + 1] - k_qsin[idx]) * f) >> 8);
}

static int isin(int a) /* Q14 */
{
    a &= 0xFFFF;
    int i = a & 0x3FFF;

    switch (a >> 14) {
    case 0:  return qsin(i);
    case 1:  return qsin(16384 - i);
    case 2:  return -qsin(i);
    default: return -qsin(16384 - i);
    }
}

static int icos(int a)
{
    return isin(a + 16384);
}

static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;

    while (b > v) {
        b >>= 2;
    }
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

/* log2(x) in Q4, x >= 1 */
static int log2_q4(uint32_t x)
{
    if (x < 1) {
        return 0;
    }
    int k = 31 - __builtin_clz(x);

    return k * 16 + (int)(((x - (1u << k)) << 4) >> k);
}

/* ---- seeded randomness (stable per shard) -------------------------------- */

static uint32_t g_rng;

static void rng_seed(const uint8_t s[8], uint32_t salt)
{
    uint32_t lo = s[0] | (s[1] << 8) | (s[2] << 16) | ((uint32_t)s[3] << 24);
    uint32_t hi = s[4] | (s[5] << 8) | (s[6] << 16) | ((uint32_t)s[7] << 24);

    g_rng = lo ^ (hi * 2654435761u) ^ (salt * 0x9E3779B9u);
    if (!g_rng) {
        g_rng = 0x6B43A9B5u;
    }
    for (int i = 0; i < 3; i++) { /* decorrelate neighbouring salts */
        g_rng ^= g_rng << 13;
        g_rng ^= g_rng >> 17;
        g_rng ^= g_rng << 5;
    }
}

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static int rnd_range(int lo, int hi)
{
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* ---- rasterizer ------------------------------------------------------------ */

typedef struct {
    surf_t s;
    int shine;   /* absolute x+y of the glint, or CRYSTAL_NO_SHINE */
    int alpha;   /* 0..256 */
    int pat;     /* mono fill pattern: domain, -1 seed shard, -2 core */
    bool light;  /* lit facet (mono: sparser pattern) */
} rctx_t;

static bool mono_ink(const rctx_t *r, int x, int y, uint16_t c)
{
    bool ink;

    switch (r->pat) {
    case AKIRA_DOM_RADIO: ink = true; break;
    case AKIRA_DOM_AIR:   ink = ((x + y) & 1) == 0; break;
    case AKIRA_DOM_NET:   ink = (y & 1) == 0; break;
    case AKIRA_DOM_WIRE:  ink = ((x + y) % 3) == 0; break;
    case AKIRA_DOM_VAULT: ink = (x & 3) == 0 && (y & 1) == 0; break;
    case AKIRA_DOM_PLAY:  ink = (x & 1) == 0 && (y & 1) == 0; break;
    default:              return surf_luma(c) < surf_bayer(x, y);
    }
    if (ink && r->light) {
        ink = ((x ^ y) & 2) == 0; /* the lit facet reads lighter */
    }
    return ink;
}

/*
 * Fill triangle a,b,c (Q4) with @p col.  Bit k of @p aa antialiases edge k
 * (0: a→b, 1: b→c, 2: c→a); the others are hard so facets meet cleanly.
 */
static void raster(const rctx_t *r, int ax, int ay, int bx, int by, int cx, int cy,
                   unsigned aa, uint16_t col)
{
    const surf_t *s = &r->s;
    int64_t area = (int64_t)(bx - ax) * (cy - ay) - (int64_t)(by - ay) * (cx - ax);

    if (area == 0 || r->alpha <= 0) {
        return;
    }
    if (area < 0) { /* make it counter-clockwise; edges 0 and 2 swap */
        int tx = bx, ty = by;

        bx = cx;
        by = cy;
        cx = tx;
        cy = ty;
        aa = ((aa & 1) << 2) | (aa & 2) | ((aa >> 2) & 1);
    }

    int minx = MAX((MIN(ax, MIN(bx, cx)) >> 4) - 1, 0);
    int maxx = MIN((MAX(ax, MAX(bx, cx)) >> 4) + 1, s->w - 1);
    int miny = MAX((MIN(ay, MIN(by, cy)) >> 4) - 1, 0);
    int maxy = MIN((MAX(ay, MAX(by, cy)) >> 4) + 1, s->h - 1);

    if (minx > maxx || miny > maxy) {
        return;
    }

    const int vx[3] = { ax, bx, cx }, vy[3] = { ay, by, cy };
    int32_t nx[3], ny[3], row[3];
    const int px0 = minx * 16 + 8, py0 = miny * 16 + 8;

    for (int k = 0; k < 3; k++) {
        int dx = vx[(k + 1) % 3] - vx[k], dy = vy[(k + 1) % 3] - vy[k];
        int len = (int)isqrt32((uint32_t)(dx * dx + dy * dy));

        if (len == 0) {
            return;
        }
        nx[k] = (int32_t)(((int64_t)-dy << 16) / len);
        ny[k] = (int32_t)(((int64_t)dx << 16) / len);
        row[k] = (int32_t)(((int64_t)nx[k] * (px0 - vx[k]) + (int64_t)ny[k] * (py0 - vy[k])) >> 4);
    }

    const bool a0 = aa & 1, a1 = aa & 2, a2 = aa & 4;

    for (int y = miny; y <= maxy; y++) {
        int32_t d0 = row[0], d1 = row[1], d2 = row[2];

        for (int x = minx; x <= maxx; x++, d0 += nx[0], d1 += nx[1], d2 += nx[2]) {
            int cov = 256;

            if (a0) {
                if (d0 <= -32768) continue;
                cov = MIN(cov, (d0 >> 8) + 128);
            } else if (d0 < 0) {
                continue;
            }
            if (a1) {
                if (d1 <= -32768) continue;
                cov = MIN(cov, (d1 >> 8) + 128);
            } else if (d1 < 0) {
                continue;
            }
            if (a2) {
                if (d2 <= -32768) continue;
                cov = MIN(cov, (d2 >> 8) + 128);
            } else if (d2 < 0) {
                continue;
            }
            if (cov <= 0) {
                continue;
            }
            cov = (cov * r->alpha) >> 8;

            uint16_t c = col;

            if (r->shine != CRYSTAL_NO_SHINE) {
                int t = x + y - r->shine;

                if (t > -14 && t < 14) {
                    c = surf_mix(c, WHITE, (14 - abs(t)) * 190 / 14);
                }
            }
            if (s->mono) {
                if (cov >= 128) {
                    px_put(s, x, y, mono_ink(r, x, y, c) ? BLACK : WHITE);
                }
            } else if (cov >= 256) {
                px_put(s, x, y, c);
            } else {
                px_put(s, x, y, surf_mix(px_get(s, x, y), c, cov));
            }
        }
        row[0] += ny[0];
        row[1] += ny[1];
        row[2] += ny[2];
    }
}

/* A thick antialiased segment as a quad (Q4 in, width in Q4). */
static void seg(const rctx_t *r, int x0, int y0, int x1, int y1, int w, uint16_t col)
{
    int dx = x1 - x0, dy = y1 - y0;
    int len = (int)isqrt32((uint32_t)(dx * dx + dy * dy));

    if (len == 0) {
        return;
    }
    int ox = -dy * w / (2 * len), oy = dx * w / (2 * len);

    raster(r, x0 + ox, y0 + oy, x1 + ox, y1 + oy, x1 - ox, y1 - oy, 0x3, col);
    raster(r, x1 - ox, y1 - oy, x0 - ox, y0 - oy, x0 + ox, y0 + oy, 0x3, col);
}

/* ---- geometry ---------------------------------------------------------------- */

#define MAX_SHARDS (10 + AKIRA_DOM_COUNT * (AKIRA_MAX_SHARDS_PER_DOMAIN + 1))
#define GOLDEN_BAU 25033 /* 137.508° */

typedef struct {
    int ang;    /* BAU, before rotation */
    int len;    /* px */
    int hw;     /* half width at the base, BAU */
    int wr;     /* shoulder half-width, % of length */
    int8_t dom; /* -1 seed shard */
    bool ghost; /* the focus domain's next shard, not real yet */
    bool newest;
    uint16_t base;
    int rank;   /* growth order */
} shard_t;

static struct {
    shard_t sh[MAX_SHARDS];
    int16_t tips[MAX_SHARDS * 2];
    int ntips;
} g AKIRA_BULK_BSS;

static uint16_t seed_colour(int ang)
{
    /* The logo's gradient: violet on the left, blue, cyan on the right. */
    static const uint16_t violet = RGB565(0x7B, 0x3F, 0xE4);
    static const uint16_t blue = RGB565(0x4F, 0x6B, 0xFF);
    static const uint16_t cyan = RGB565(0x3F, 0xD0, 0xF2);
    int t = (16384 + icos(ang)) >> 7; /* 0 (left) .. 256 (right) */

    return t < 128 ? surf_mix(violet, blue, t * 2) : surf_mix(blue, cyan, (t - 128) * 2);
}

static int layout(const akira_crystal_view_t *v, const crystal_fx_t *fx, int R)
{
    int n = 0;

    rng_seed(v->seed, 0xC0FEu);
    const int nb = 7 + (int)(rnd() % 3);
    const int base_ang = (int)(rnd() & 0xFFFF);
    const int step = CRYSTAL_TURN / nb;

    for (int i = 0; i < nb; i++) {
        shard_t *s = &g.sh[n++];

        rng_seed(v->seed, 1000 + i);
        memset(s, 0, sizeof(*s));
        s->ang = base_ang + i * step + rnd_range(-step / 6, step / 6);
        s->len = R * ((i & 1) ? rnd_range(48, 60) : rnd_range(60, 74)) / 100;
        s->hw = rnd_range(800, 1300);
        s->wr = rnd_range(15, 22);
        s->dom = -1;
        s->base = seed_colour(s->ang);
    }
    for (int d = 0; d < AKIRA_DOM_COUNT; d++) {
        int real = MIN(v->shards[d], AKIRA_MAX_SHARDS_PER_DOMAIN);
        int count = real + ((fx->ghost > 0 && fx->focus == d && real < AKIRA_MAX_SHARDS_PER_DOMAIN)
                                ? 1 : 0);
        uint32_t per = v->uses[d] / (uint32_t)MAX(real, 1);
        int reach = MIN(55 + 9 * log2_q4(1 + per) / 16, 100);

        for (int i = 0; i < count && n < MAX_SHARDS; i++) {
            shard_t *s = &g.sh[n++];
            int k = i * AKIRA_DOM_COUNT + d;

            rng_seed(v->seed, 2000 + d * 16 + i);
            memset(s, 0, sizeof(*s));
            s->ang = base_ang + step / 2 + k * GOLDEN_BAU + rnd_range(-700, 700);
            s->len = R * CLAMP(reach + rnd_range(-6, 6), 40, 100) / 100;
            s->hw = rnd_range(700, 1100);
            s->wr = rnd_range(12, 17);
            s->dom = (int8_t)d;
            s->base = akira_domain_colour(d);
            s->ghost = (i >= real);
            s->newest = (i == real - 1);
        }
    }
    /* Growth sweeps round the crystal: rank by angle from the first shard. */
    for (int i = 0; i < n; i++) {
        int rank = 0;
        int ai = (g.sh[i].ang - base_ang) & 0xFFFF;

        for (int j = 0; j < n; j++) {
            int aj = (g.sh[j].ang - base_ang) & 0xFFFF;

            if (aj < ai || (aj == ai && j < i)) {
                rank++;
            }
        }
        g.sh[i].rank = rank;
    }
    return n;
}

/* Growth timing (ms). */
#define T_CORE   360
#define T_SHARD0 200
#define D_SHARD  460

static int shard_stagger(int n)
{
    return CLAMP(900 / MAX(n, 1), 25, 70);
}

int crystal_grow_duration(const akira_crystal_view_t *v)
{
    int n = 9;

    for (int d = 0; d < AKIRA_DOM_COUNT; d++) {
        n += v->shards[d];
    }
    return T_SHARD0 + n * shard_stagger(n) + D_SHARD + MIN(v->glints, 12) * 50 + 300;
}

void crystal_fx_init(crystal_fx_t *fx, int cx, int cy, int radius, uint16_t bg)
{
    memset(fx, 0, sizeof(*fx));
    fx->cx = cx;
    fx->cy = cy;
    fx->radius = radius;
    fx->grow_ms = CRYSTAL_GROWN;
    fx->shine = CRYSTAL_NO_SHINE;
    fx->focus = AKIRA_DOM_NONE;
    fx->regrow_ms = -1;
    fx->bg = bg;
}

/* Eased 0..256 (cubic out / back out), with overshoot for back. */
static int ease_out(int t, int dur)
{
    if (t <= 0) return 0;
    if (t >= dur) return 256;
    int u = 256 - t * 256 / dur;

    return 256 - (u * u / 256) * u / 256;
}

static int ease_back(int t, int dur)
{
    if (t <= 0) return 0;
    if (t >= dur) return 256;
    int u = t * 256 / dur - 256;
    int u2 = u * u / 256, u3 = u2 * u / 256;

    return 256 + (692 * u3) / 256 + (436 * u2) / 256;
}

static void polar(int cx, int cy, int r_q4, int ang, int *x, int *y)
{
    *x = cx + (int)(((int64_t)r_q4 * icos(ang)) >> 14);
    *y = cy + (int)(((int64_t)r_q4 * isin(ang)) >> 14);
}

/* ---- drawing ------------------------------------------------------------------ */

static uint16_t finish_colour(const crystal_fx_t *fx, uint16_t c, int dom, bool core)
{
    if (fx->focus != AKIRA_DOM_NONE) {
        if (!core && dom == fx->focus) {
            c = surf_mix(c, WHITE, fx->focus_glow * 150 / 256);
        } else {
            c = surf_mix(c, fx->bg, fx->dim);
        }
    }
    return c;
}

static void draw_crack(rctx_t *r, int cx, int cy, int len_q4, int ang)
{
    static const int at[4] = { 30, 46, 62, 80 }; /* % along the shard */
    static const int off[4] = { -1, 1, -1, 1 };
    int px[4], py[4];

    for (int i = 0; i < 4; i++) {
        int along = len_q4 * at[i] / 100;
        int side = 30 * off[i] * (100 - at[i]) / 70; /* ~2 px zigzag, tapering */

        polar(cx, cy, along, ang, &px[i], &py[i]);
        px[i] += (int)(((int64_t)side * icos(ang + 16384)) >> 14);
        py[i] += (int)(((int64_t)side * isin(ang + 16384)) >> 14);
    }
    rctx_t rr = *r;

    rr.pat = AKIRA_DOM_RADIO; /* solid ink on mono */
    rr.light = false;
    for (int i = 0; i < 3; i++) {
        seg(&rr, px[i], py[i], px[i + 1], py[i + 1], 22, RGB565(0x0E, 0x10, 0x26));
        seg(&rr, px[i] + 10, py[i] - 10, px[i + 1] + 10, py[i + 1] - 10, 10,
            RGB565(0xF0, 0xF4, 0xFF));
    }
}

void crystal_draw(const akira_crystal_view_t *v, const crystal_fx_t *fx)
{
    rctx_t r;

    if (!surf_open(&r.s) || fx->radius <= 0) {
        return;
    }
    const int R = fx->radius;
    const int n = layout(v, fx, R);
    const int cx = fx->cx * 16 + 8, cy = fx->cy * 16 + 8;
    const int stagger = shard_stagger(n);
    const int t = fx->grow_ms;
    const int light_dir = 40960; /* 225°: up and to the left */

    r.shine = fx->shine == CRYSTAL_NO_SHINE ? CRYSTAL_NO_SHINE
                                            : (fx->cx - R) + (fx->cy - R) + fx->shine;
    r.alpha = 256;

    /* Core size grows with the days Akira has been with you. */
    int rc = R * MIN(26 + 25 * log2_q4(1u + v->days) / 160, 36) / 100;
    int core_p = (t >= CRYSTAL_GROWN) ? 256 : ease_back(t, T_CORE);
    int rc_q4 = rc * 16 * core_p / 256;

    g.ntips = 0;

    /* Shards: seed shards behind, domain shards in front. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            const shard_t *s = &g.sh[i];

            if ((pass == 0) != (s->dom < 0)) {
                continue;
            }
            int p = 256, hot = 0;

            if (t < CRYSTAL_GROWN) {
                int st = t - T_SHARD0 - s->rank * stagger;

                p = ease_back(st, D_SHARD);
                hot = (256 - ease_out(st, D_SHARD)) * 220 / 256;
                if (p <= 0) {
                    continue;
                }
            }
            if (s->newest && fx->regrow_ms >= 0 && s->dom == fx->focus) {
                p = ease_back(fx->regrow_ms, D_SHARD);
                hot = MAX(hot, (256 - ease_out(fx->regrow_ms, D_SHARD + 300)) * 240 / 256);
                if (p <= 0) {
                    continue;
                }
            }

            int len_q4 = s->len * 16 * p / 256;
            int alpha = 256;
            uint16_t c = s->base;

            if (s->ghost) {
                /* Not yet real: a translucent spike that fills with the ring. */
                len_q4 = len_q4 * (90 + fx->ghost * 166 / 256) / 256;
                alpha = 70 + fx->ghost * 150 / 256;
                hot = MAX(hot, fx->ghost * 200 / 256);
            }
            c = surf_mix(c, WHITE, hot);
            c = finish_colour(fx, c, s->dom, false);

            const int a = s->ang + fx->angle;
            const int rb = MAX(rc * 16 * 85 / 100, 3 * 16);
            int tx, ty, lx, ly, rx, ry, mx, my, ax, ay, slx, sly, srx, sry;

            /* A cut crystal, not a needle: narrow at the core, widest at a
             * shoulder ~38% out, then a long point. */
            polar(cx, cy, len_q4, a, &tx, &ty);
            polar(cx, cy, rb, a - s->hw, &lx, &ly);
            polar(cx, cy, rb, a + s->hw, &rx, &ry);
            polar(cx, cy, rb / 2, a, &mx, &my);
            polar(cx, cy, MAX(len_q4 * 38 / 100, rb + 16), a, &ax, &ay);

            int ws = MAX(len_q4 * s->wr / 100, 24);

            polar(ax, ay, ws, a - 16384, &slx, &sly);
            polar(ax, ay, ws, a + 16384, &srx, &sry);

            bool left_lit = icos(a - 16384 - light_dir) > icos(a + 16384 - light_dir);
            uint16_t lit = surf_mix(c, WHITE, 84), shade = surf_mix(c, BLACK, 64);
            uint16_t lit_tip = surf_mix(lit, WHITE, 46), shade_tip = surf_mix(shade, c, 90);

            if (fx->flash) {
                lit = surf_mix(lit, WHITE, fx->flash);
                shade = surf_mix(shade, WHITE, fx->flash);
                lit_tip = surf_mix(lit_tip, WHITE, fx->flash);
                shade_tip = surf_mix(shade_tip, WHITE, fx->flash);
            }
            r.alpha = alpha;
            r.pat = s->dom;
            /* left half: base facet + tip facet */
            r.light = left_lit;
            raster(&r, mx, my, lx, ly, slx, sly, 0x2, left_lit ? lit : shade);
            raster(&r, mx, my, slx, sly, tx, ty, 0x2, left_lit ? lit_tip : shade_tip);
            /* right half */
            r.light = !left_lit;
            raster(&r, mx, my, srx, sry, rx, ry, 0x2, left_lit ? shade : lit);
            raster(&r, mx, my, tx, ty, srx, sry, 0x2, left_lit ? shade_tip : lit_tip);

            if (!s->ghost && g.ntips < MAX_SHARDS) {
                g.tips[g.ntips * 2] = (int16_t)(tx >> 4);
                g.tips[g.ntips * 2 + 1] = (int16_t)(ty >> 4);
                g.ntips++;
            }
        }
    }

    /* Core: a faceted fan, brightest at the heart. */
    if (rc_q4 > 0) {
        rng_seed(v->seed, 0xC08Eu);
        const int nc = 6 + (int)(rnd() % 3);
        int vx[9], vy[9];
        const uint16_t pale = RGB565(0xBC, 0xC8, 0xFF), deep = RGB565(0x7E, 0x8E, 0xFF);

        for (int i = 0; i < nc; i++) {
            int ang = fx->angle + i * CRYSTAL_TURN / nc + rnd_range(-1800, 1800);
            int rr = rc_q4 * rnd_range(82, 100) / 100;

            polar(cx, cy, rr, ang, &vx[i], &vy[i]);
        }
        for (int i = 0; i < nc; i++) {
            int j = (i + 1) % nc;
            uint16_t c = (i % 3 == 0) ? surf_mix(pale, WHITE, 140)
                         : (i % 3 == 1) ? pale : surf_mix(pale, deep, 120);

            c = finish_colour(fx, c, AKIRA_DOM_NONE, true);
            c = surf_mix(c, WHITE, fx->flash);
            r.alpha = 256;
            r.pat = -2;
            r.light = (i % 3 == 0);
            raster(&r, cx, cy, vx[i], vy[i], vx[j], vy[j], 0x2, c);
        }
    }

    /* Cracks: across the first shard of each cracked domain (or the core). */
    if (t >= CRYSTAL_GROWN) {
        for (int d = 0; d < AKIRA_DOM_COUNT; d++) {
            if (!v->cracks[d]) {
                continue;
            }
            const shard_t *target = NULL;

            for (int i = 0; i < n; i++) {
                if (g.sh[i].dom == d && !g.sh[i].ghost) {
                    target = &g.sh[i];
                    break;
                }
            }
            r.alpha = 256;
            if (target) {
                draw_crack(&r, cx, cy, target->len * 16, target->ang + fx->angle);
            } else if (rc > 0) {
                draw_crack(&r, cx, cy, rc * 16 * 2, fx->angle + d * 10923);
            }
        }
    }

    /* Glints: achievements, floating just beyond the shards. */
    int ng = MIN(v->glints, 12);
    int glint0 = T_SHARD0 + n * stagger + D_SHARD;

    for (int i = 0; i < ng; i++) {
        int p = 256;

        if (t < CRYSTAL_GROWN) {
            p = ease_back(t - glint0 - i * 50, 300);
            if (p <= 0) {
                continue;
            }
        }
        rng_seed(v->seed, 3000 + i);
        int ang = fx->angle + (int)(rnd() & 0xFFFF);
        int rg = R * 16 * rnd_range(96, 114) / 100;
        int sz = rnd_range(28, 52) * p / 256; /* Q4 half-size */
        int gx, gy;

        polar(cx, cy, rg, ang, &gx, &gy);
        uint16_t c = surf_mix(akira_domain_colour(AKIRA_DOM_PLAY), WHITE, 110);

        c = finish_colour(fx, c, AKIRA_DOM_PLAY, false);
        c = surf_mix(c, WHITE, fx->flash);
        r.alpha = 256;
        r.pat = AKIRA_DOM_PLAY;
        r.light = true;
        raster(&r, gx, gy - sz, gx + sz * 2 / 3, gy, gx, gy + sz, 0x3, c);
        r.light = false;
        raster(&r, gx, gy + sz, gx - sz * 2 / 3, gy, gx, gy - sz, 0x3,
               surf_mix(c, BLACK, 50));
    }
}

void crystal_draw_arc(int cx, int cy, int r, int width, int from, int to, uint16_t colour)
{
    rctx_t rc;

    if (!surf_open(&rc.s) || to <= from) {
        return;
    }
    rc.shine = CRYSTAL_NO_SHINE;
    rc.alpha = 256;
    rc.pat = AKIRA_DOM_RADIO;
    rc.light = false;

    const int x0 = cx * 16 + 8, y0 = cy * 16 + 8, R = r * 16, w = width * 16;
    const int step = CRYSTAL_TURN / 72; /* 5° chords: no visible facets at this size */
    int px, py, qx, qy;

    polar(x0, y0, R, from, &px, &py);
    for (int a = from; a < to; a += step) {
        int b = MIN(a + step, to);

        polar(x0, y0, R, b, &qx, &qy);
        seg(&rc, px, py, qx, qy, w, colour);
        px = qx;
        py = qy;
    }
}

int crystal_tips(int16_t *xy, int max)
{
    int n = MIN(g.ntips, max);

    memcpy(xy, g.tips, sizeof(int16_t) * 2 * n);
    return n;
}

/* ---- sigil ---------------------------------------------------------------------- */

static void ring(const surf_t *s, int cx, int cy, int r_q4, int w_q4, uint16_t col)
{
    const int outer = r_q4 + w_q4 / 2 + 16;
    const int ya = MAX((cy - outer) >> 4, 0), yb = MIN((cy + outer) >> 4, s->h - 1);
    const int xa = MAX((cx - outer) >> 4, 0), xb = MIN((cx + outer) >> 4, s->w - 1);

    for (int y = ya; y <= yb; y++) {
        int dy = y * 16 + 8 - cy;

        for (int x = xa; x <= xb; x++) {
            int dx = x * 16 + 8 - cx;
            int d = (int)isqrt32((uint32_t)(dx * dx + dy * dy));
            int edge = w_q4 / 2 - abs(d - r_q4); /* Q4 px inside the band */

            if (edge > -8) {
                surf_blend(s, x, y, col, CLAMP((edge + 8) * 16, 0, 256));
            }
        }
    }
}

void crystal_draw_sigil(int cx, int cy, int r, const uint8_t seed[16], uint16_t colour)
{
    rctx_t rc;

    if (!surf_open(&rc.s) || r < 6) {
        return;
    }
    rc.shine = CRYSTAL_NO_SHINE;
    rc.alpha = 256;
    rc.pat = AKIRA_DOM_RADIO;
    rc.light = false;

    const int x0 = cx * 16 + 8, y0 = cy * 16 + 8, R = r * 16;
    const int w = r >= 28 ? 36 : 22; /* stroke, Q4 */

    ring(&rc.s, x0, y0, R, w, colour);
    ring(&rc.s, x0, y0, R * 78 / 100, w / 2 + 6, colour);

    /* 24 tick marks between the rings, one per seed bit. */
    uint32_t ticks = seed[0] | (seed[1] << 8) | ((uint32_t)seed[2] << 16);

    for (int k = 0; k < 24; k++) {
        if (!(ticks & BIT(k))) {
            continue;
        }
        int a = k * CRYSTAL_TURN / 24, xa, ya, xb, yb;

        polar(x0, y0, R * 83 / 100, a, &xa, &ya);
        polar(x0, y0, R * 94 / 100, a, &xb, &yb);
        seg(&rc, xa, ya, xb, yb, w / 2 + 4, colour);
    }

    /* The rune: strokes on a 3x3 lattice, mirrored left↔right. */
    static const uint8_t cand[12][4] = {
        { 0, 0, 1, 0 }, { 0, 0, 0, 1 }, { 0, 1, 0, 2 }, { 0, 2, 1, 2 },
        { 0, 0, 1, 1 }, { 0, 2, 1, 1 }, { 0, 1, 1, 1 }, { 1, 0, 1, 1 },
        { 1, 1, 1, 2 }, { 0, 1, 1, 0 }, { 0, 1, 1, 2 }, { 1, 0, 1, 2 },
    };
    uint16_t pick = (uint16_t)(seed[3] | (seed[4] << 8)) & 0x0FFF;

    if (__builtin_popcount(pick) < 3) {
        pick |= (uint16_t)(0x0111u << (seed[5] % 4));
    }
    const int lx = R * 30 / 100, ly = R * 34 / 100;

    for (int i = 0; i < 12; i++) {
        if (!(pick & BIT(i))) {
            continue;
        }
        for (int m = 0; m < 2; m++) {
            int c0 = m ? 2 - cand[i][0] : cand[i][0];
            int c1 = m ? 2 - cand[i][2] : cand[i][2];

            seg(&rc, x0 + (c0 - 1) * lx, y0 + (cand[i][1] - 1) * ly, x0 + (c1 - 1) * lx,
                y0 + (cand[i][3] - 1) * ly, w, colour);
        }
    }
    if (seed[6] & 1) {
        ring(&rc.s, x0, y0, 0, w + 8, colour); /* a centre stud */
    }
}
