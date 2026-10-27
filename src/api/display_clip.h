/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_DISPLAY_CLIP_H
#define AKIRA_DISPLAY_CLIP_H

/**
 * @file display_clip.h
 * @brief Shared screen-rectangle clipping for the display primitives.
 *
 * Every framebuffer primitive that takes a rectangle needs the same clip:
 * intersect the requested rect with the panel, and draw nothing if the result
 * is empty.  That logic used to be copy-pasted into akira_display_rect(),
 * akira_display_bitmap() and akira_display_bitmap_transparent() — and one copy
 * lost the empty-result check, which is how CVE-class out-of-bounds writes get
 * introduced:
 *
 *     int span = x2 - x1;                                 // negative when
 *     memcpy(dst, src, (size_t)span * sizeof(uint16_t));  // fully off-screen
 *
 * A negative span casts to a huge size_t and the memcpy runs off the end of the
 * framebuffer.  Coordinates reach these primitives straight from WASM apps as
 * attacker-controlled int32_t, so this was reachable by any app holding the
 * display.write capability.
 *
 * Keep this the single source of truth: new primitives clip through here rather
 * than open-coding MAX/MIN, so the guard cannot be forgotten again.
 *
 * Header-only and dependency-free on purpose — it compiles into the native_sim
 * ztest build (where CONFIG_DISPLAY=n) without dragging in the display stack.
 */

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Intersect a rectangle with the screen.
 *
 * @param x,y,w,h     Requested rectangle. Fully attacker-controlled; any
 *                    int32_t value must be handled without UB.
 * @param xres,yres   Panel resolution.
 * @param[out] x1,y1  Top-left of the clipped rect (inclusive). Untouched when
 *                    the function returns false.
 * @param[out] x2,y2  Bottom-right of the clipped rect (exclusive). Untouched
 *                    when the function returns false.
 *
 * @return true if a non-empty rect remains and the caller should draw;
 *         false if it is fully clipped and the caller must draw nothing.
 *
 * @note Partially off-screen rectangles are clipped, not rejected — drawing a
 *       sprite that hangs off the left edge is legitimate and callers rely on
 *       it.  Only fully-off-screen and degenerate rects return false.
 */
static inline bool akira_display_clip_rect(int x, int y, int w, int h,
                                           int xres, int yres,
                                           int *x1, int *y1, int *x2, int *y2)
{
    if (w <= 0 || h <= 0 || xres <= 0 || yres <= 0) {
        return false;
    }

    /* int64_t throughout: x + w overflows for x near INT32_MAX, and signed
     * overflow is UB — the compiler is entitled to assume it cannot happen and
     * optimise the bounds check away entirely. */
    int64_t left   = (int64_t)x;
    int64_t top    = (int64_t)y;
    int64_t right  = (int64_t)x + (int64_t)w;
    int64_t bottom = (int64_t)y + (int64_t)h;

    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > (int64_t)xres) {
        right = xres;
    }
    if (bottom > (int64_t)yres) {
        bottom = yres;
    }

    /* Empty after clipping: the rect is entirely off one side of the panel.
     * This is the check whose absence caused the negative-span memcpy. */
    if (right <= left || bottom <= top) {
        return false;
    }

    *x1 = (int)left;
    *y1 = (int)top;
    *x2 = (int)right;
    *y2 = (int)bottom;
    return true;
}

#endif /* AKIRA_DISPLAY_CLIP_H */
