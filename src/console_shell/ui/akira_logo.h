/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_logo.h
 * @brief The AkiraOS crystal logo, plus the light effects the boot splash
 *        builds around it.
 *
 * Bitmaps come from assets/logo.png via tools/gen_logo.py (RGB565 + 4-bit
 * alpha; the large one also carries a growth-order map).  Everything is
 * alpha-blended over whatever is already in the framebuffer, so the logo sits
 * on the dark splash, the white shell, or a card alike.
 *
 * On a 1-bit panel (Sharp LS0XX) colours can't survive the MONO conversion —
 * every non-black pixel turns white — so the logo is ordered-dithered to ink
 * instead, and the glow is skipped.
 */

#ifndef AKIRA_SHELL_LOGO_H
#define AKIRA_SHELL_LOGO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AKIRA_LOGO_LARGE = 0, /**< 113x120 — boot splash hero (has growth map) */
    AKIRA_LOGO_SMALL,     /**< 30x32  — wait screen, system info */
} akira_logo_size_t;

/** Width of the hot, white-hot growth front, in growth-order units. */
#define AKIRA_LOGO_EDGE 28
/** fx.reveal at or past this draws the finished logo (front gone). */
#define AKIRA_LOGO_REVEAL_END (255 + AKIRA_LOGO_EDGE)
/** fx.shine value meaning "no glint". */
#define AKIRA_LOGO_NO_SHINE (-32768)

typedef struct {
    /** Growth front 0..AKIRA_LOGO_REVEAL_END: pixels whose growth order is
     *  behind it are drawn, the last AKIRA_LOGO_EDGE units glowing white.
     *  Ignored by sizes without a growth map. */
    int reveal;
    /** Centre of a diagonal glint, measured along (x + y) in logo pixels:
     *  sweep it from about -20 to w + h + 20.  AKIRA_LOGO_NO_SHINE = off. */
    int shine;
    /** 0..256 */
    int opacity;
    /** Q8 scale, 256 = 1:1 (nearest-neighbour). */
    int scale;
    /** Uniform whitening 0..256 — the crystal flaring to pure light. */
    int flash;
} akira_logo_fx_t;

/** Finished logo at full opacity and size. */
#define AKIRA_LOGO_FX_STATIC \
    ((akira_logo_fx_t){ AKIRA_LOGO_REVEAL_END, AKIRA_LOGO_NO_SHINE, 256, 256, 0 })

int akira_logo_w(akira_logo_size_t size);
int akira_logo_h(akira_logo_size_t size);

/** Draw the logo centred on (@p cx, @p cy).  @p fx NULL = static. */
void akira_logo_draw(akira_logo_size_t size, int cx, int cy, const akira_logo_fx_t *fx);

/** Draw the static logo with its top-left at (@p x, @p y). */
void akira_logo_draw_at(akira_logo_size_t size, int x, int y);

/**
 * Soft radial light: blend @p colour over the frame around (@p cx, @p cy),
 * strongest at the centre and fading to nothing at radius @p r.  Ordered
 * dithering keeps the RGB565 gradient from banding.  @p strength 0..256.
 */
void akira_logo_glow(int cx, int cy, int r, uint16_t colour, int strength);

/**
 * Fill a disc of @p colour of radius @p r around (@p cx, @p cy), its rim
 * fading into the frame over @p soft px (dithered, no Bresenham gaps).
 */
void akira_logo_burst(int cx, int cy, int r, int soft, uint16_t colour);

/** A four-point twinkle of half-length @p size (1..8 looks right). */
void akira_logo_sparkle(int cx, int cy, int size, uint16_t colour);

/** RGB565 linear mix: @p t 0 = @p a, 256 = @p b. */
uint16_t akira_logo_mix(uint16_t a, uint16_t b, int t);

/** True on a 1-bit panel — callers pick an ink-on-paper palette there. */
bool akira_logo_panel_is_mono(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SHELL_LOGO_H */
