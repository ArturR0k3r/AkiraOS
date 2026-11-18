/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_DISPLAY_CLIP_H
#define AKIRA_DISPLAY_CLIP_H

/**
 * @file display_clip.h
 * @brief Shared screen clipping for the display primitives.
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
 * Lines need clipping too, for a different reason: a rasteriser walks every
 * point of the requested segment, so an unclipped line from INT32_MIN to
 * INT32_MAX is four billion steps inside one native call.
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
 * @brief Intersect a box given by its edges with the screen.
 *
 * Same contract as akira_display_clip_rect(), for a box [x0, x1) x [y0, y1)
 * whose edges are already 64-bit: shapes derived from app coordinates
 * (x + w, x + w - radius, ...) overflow int before they can be clipped.
 *
 * @param x0,y0       Top-left (inclusive).
 * @param x1,y1       Bottom-right (exclusive).
 * @param xres,yres   Panel resolution.
 * @param[out] ox0,oy0,ox1,oy1  Clipped box, same convention. Untouched when
 *                    the function returns false.
 *
 * @return true if a non-empty box remains and the caller should draw.
 */
static inline bool akira_display_clip_box(int64_t x0, int64_t y0,
                                          int64_t x1, int64_t y1,
                                          int xres, int yres,
                                          int *ox0, int *oy0,
                                          int *ox1, int *oy1)
{
    if (xres <= 0 || yres <= 0) {
        return false;
    }

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (int64_t)xres) {
        x1 = xres;
    }
    if (y1 > (int64_t)yres) {
        y1 = yres;
    }

    /* Empty after clipping: the box is entirely off one side of the panel.
     * This is the check whose absence caused the negative-span memcpy. */
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }

    *ox0 = (int)x0;
    *oy0 = (int)y0;
    *ox1 = (int)x1;
    *oy1 = (int)y1;
    return true;
}

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
    if (w <= 0 || h <= 0) {
        return false;
    }

    /* int64_t: x + w overflows for x near INT32_MAX, and signed overflow is
     * UB — the compiler is entitled to assume it cannot happen and optimise
     * the bounds check away entirely. */
    return akira_display_clip_box((int64_t)x, (int64_t)y,
                                  (int64_t)x + (int64_t)w,
                                  (int64_t)y + (int64_t)h,
                                  xres, yres, x1, y1, x2, y2);
}

/**
 * Vertex coordinates are clamped to +-this before any interpolation, so the
 * product of two coordinate differences (each < 2^31) stays inside int64.
 * Far beyond any panel: the clamp only bends segments that run off-screen.
 */
#define AKIRA_DISPLAY_VERTEX_LIM (1 << 29)

/** Clamp an app-supplied vertex coordinate to +-AKIRA_DISPLAY_VERTEX_LIM. */
static inline int64_t akira_display_clamp_vertex(int64_t v)
{
    if (v < -(int64_t)AKIRA_DISPLAY_VERTEX_LIM) {
        return -(int64_t)AKIRA_DISPLAY_VERTEX_LIM;
    }
    if (v > (int64_t)AKIRA_DISPLAY_VERTEX_LIM) {
        return AKIRA_DISPLAY_VERTEX_LIM;
    }
    return v;
}

/* Cohen-Sutherland outcode of (x, y) against [0, xmax] x [0, ymax]. */
static inline int akira_display__outcode(int64_t x, int64_t y,
                                         int64_t xmax, int64_t ymax)
{
    int c = 0;

    if (x < 0) {
        c |= 1; /* left */
    } else if (x > xmax) {
        c |= 2; /* right */
    }
    if (y < 0) {
        c |= 4; /* above the top row */
    } else if (y > ymax) {
        c |= 8; /* below the bottom row */
    }
    return c;
}

/**
 * @brief Clip a line segment to the screen (Cohen-Sutherland).
 *
 * @param x0,y0,x1,y1  End points, both inclusive. Any int32_t value; each is
 *                     first clamped with akira_display_clamp_vertex().
 * @param xres,yres    Panel resolution.
 * @param[out] ox0,oy0,ox1,oy1  Clipped end points, all on the panel
 *                     (0 <= x < xres, 0 <= y < yres). Untouched when the
 *                     function returns false.
 *
 * @return true if part of the segment is visible. A rasteriser walking the
 *         result then takes at most max(xres, yres) steps, however long the
 *         requested segment was. Segments that are fully on-screen come back
 *         unchanged.
 */
static inline bool akira_display_clip_line(int x0, int y0, int x1, int y1,
                                           int xres, int yres,
                                           int *ox0, int *oy0,
                                           int *ox1, int *oy1)
{
    if (xres <= 0 || yres <= 0) {
        return false;
    }

    const int64_t xmax = (int64_t)xres - 1;
    const int64_t ymax = (int64_t)yres - 1;
    int64_t ax = akira_display_clamp_vertex(x0);
    int64_t ay = akira_display_clamp_vertex(y0);
    int64_t bx = akira_display_clamp_vertex(x1);
    int64_t by = akira_display_clamp_vertex(y1);
    int ca = akira_display__outcode(ax, ay, xmax, ymax);
    int cb = akira_display__outcode(bx, by, xmax, ymax);

    /* Exact arithmetic needs at most two clips per end; integer rounding can
     * add a step.  The cap keeps the loop bounded whatever the rounding does
     * (worst case: a segment grazing a corner is dropped). */
    for (int iter = 0; iter < 8; iter++) {
        if (!(ca | cb)) {
            *ox0 = (int)ax;
            *oy0 = (int)ay;
            *ox1 = (int)bx;
            *oy1 = (int)by;
            return true;
        }
        if (ca & cb) {
            return false; /* both ends beyond the same edge */
        }

        /* The chosen bit is set for one end only, so the delta along that
         * axis is non-zero.  |delta| <= 2^30 and |edge - a| <= 2^29 + xres,
         * so each product fits int64. */
        int c = ca ? ca : cb;
        int64_t dx = bx - ax;
        int64_t dy = by - ay;
        int64_t x, y;

        if (c & 8) {
            y = ymax;
            x = ax + dx * (ymax - ay) / dy;
        } else if (c & 4) {
            y = 0;
            x = ax + dx * (0 - ay) / dy;
        } else if (c & 2) {
            x = xmax;
            y = ay + dy * (xmax - ax) / dx;
        } else {
            x = 0;
            y = ay + dy * (0 - ax) / dx;
        }

        if (c == ca) {
            ax = x;
            ay = y;
            ca = akira_display__outcode(ax, ay, xmax, ymax);
        } else {
            bx = x;
            by = y;
            cb = akira_display__outcode(bx, by, xmax, ymax);
        }
    }
    return false;
}

#endif /* AKIRA_DISPLAY_CLIP_H */
