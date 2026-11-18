/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file crystal_render.h
 * @brief Draws Akira — the procedural low-poly crystal — and seal sigils.
 *
 * The shape is a pure function of the crystal's state and seed: a core,
 * 7-9 seed shards in the logo's purple→cyan gradient, then one shard per
 * sealed (app, power) in its domain colour.  New shards take golden-angle
 * slots, so growth never rearranges what is already there.
 *
 * Facets are flat-shaded (light from the top-left) and only the silhouette
 * edges are antialiased, so neighbouring facets never show a seam.
 */

#ifndef AKIRA_SHELL_CRYSTAL_RENDER_H
#define AKIRA_SHELL_CRYSTAL_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include <crystal/akira_crystal.h>

#ifdef __cplusplus
extern "C" {
#endif

/** fx.grow_ms value meaning "fully grown". */
#define CRYSTAL_GROWN (1 << 24)
/** fx.shine value meaning "no glint". */
#define CRYSTAL_NO_SHINE (-32768)
/** Binary angle units: a full turn. */
#define CRYSTAL_TURN 65536

typedef struct {
    int cx, cy;
    int radius;     /**< px reach of a fully grown shard */
    int angle;      /**< rotation, CRYSTAL_TURN = 360° */
    int grow_ms;    /**< ms into the growth animation, or CRYSTAL_GROWN */
    int flash;      /**< 0..256 — the crystal flaring to pure light */
    int shine;      /**< glint centre along (x + y) relative to the crystal's
                     *   top-left corner, 0..4*radius; CRYSTAL_NO_SHINE = off */
    int focus;      /**< domain to light up (seal screen); AKIRA_DOM_NONE */
    int focus_glow; /**< 0..256 */
    int dim;        /**< 0..256 — fade everything but the focus toward bg */
    int ghost;      /**< 0..256 — the focus domain's next shard fills in */
    int regrow_ms;  /**< >= 0: the newest focus shard grows again; -1 none */
    uint16_t bg;    /**< what the crystal sits on (for dimming) */
} crystal_fx_t;

/** Fully grown, still, no effects. */
void crystal_fx_init(crystal_fx_t *fx, int cx, int cy, int radius, uint16_t bg);

/** Length of the growth animation for this crystal, in ms. */
int crystal_grow_duration(const akira_crystal_view_t *v);

/** Draw Akira over the current frame. */
void crystal_draw(const akira_crystal_view_t *v, const crystal_fx_t *fx);

/**
 * Antialiased arc of @p width px on radius @p r, from angle @p from to
 * @p to (CRYSTAL_TURN units, 0 = 3 o'clock, clockwise on screen).
 */
void crystal_draw_arc(int cx, int cy, int r, int width, int from, int to, uint16_t colour);

/** Shard tips of the last crystal_draw() as x,y pairs; returns the count. */
int crystal_tips(int16_t *xy, int max);

/**
 * A seal's sigil: two rings, 24 tick marks and a mirrored rune, all chosen
 * by @p seed — every seal looks different, like a wax impression.
 */
void crystal_draw_sigil(int cx, int cy, int r, const uint8_t seed[16], uint16_t colour);

/** 20x16 1-bit home-tile icon (bit 31 = leftmost). */
extern const uint32_t crystal_icon[16];

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SHELL_CRYSTAL_RENDER_H */
