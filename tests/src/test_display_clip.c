/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file test_display_clip.c
 * @brief Regression tests for akira_display_clip_rect().
 *
 * Guards the fix for the out-of-bounds write in akira_display_bitmap(): the
 * clipped span was computed but never checked for being empty, so a fully
 * off-screen rectangle produced a negative span which, cast to size_t, became a
 * ~4 GB memcpy into the framebuffer.  Coordinates reach that code as
 * attacker-controlled int32_t from any WASM app holding display.write.
 *
 * The vulnerable code lives behind #if !AKIRA_PLATFORM_NATIVE_SIM, so it cannot
 * be driven directly from a native_sim ztest.  Testing the shared clip helper
 * instead is what makes the guard reachable here — which is the main reason the
 * clip logic was extracted rather than patched in place.
 *
 * Two properties matter and pull in opposite directions:
 *   1. fully off-screen / degenerate  -> false (draw nothing)
 *   2. PARTIALLY off-screen           -> true, with clipped bounds
 * Getting (1) by rejecting all negative coordinates would break (2), which is
 * legitimate and relied upon by callers drawing sprites that hang off an edge.
 */

#include <zephyr/ztest.h>
#include <stdint.h>

#include <api/display_clip.h>

/* AkiraConsole Production panel. */
#define XRES 320
#define YRES 240

ZTEST_SUITE(display_clip, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* Fully clipped — the cases that caused the OOB write                  */
/* ------------------------------------------------------------------ */

/* The primary trigger. Note x is POSITIVE: a fix that only rejected negative
 * coordinates would leave this exploitable.
 * Pre-fix: x1 = 400, x2 = 320, span = -80 -> (size_t)(-80) * 2 byte memcpy. */
ZTEST(display_clip, test_reject_beyond_right_edge)
{
    int x1, y1, x2, y2;

    zassert_false(akira_display_clip_rect(400, 0, 10, 10, XRES, YRES,
                                          &x1, &y1, &x2, &y2),
                  "rect starting past the right edge must be fully clipped");
}

/* Pre-fix: x1 = 0, x2 = -50, span = -50. */
ZTEST(display_clip, test_reject_beyond_left_edge)
{
    int x1, y1, x2, y2;

    zassert_false(akira_display_clip_rect(-100, 0, 50, 10, XRES, YRES,
                                          &x1, &y1, &x2, &y2),
                  "rect ending before the left edge must be fully clipped");
}

ZTEST(display_clip, test_reject_beyond_vertical_edges)
{
    int x1, y1, x2, y2;

    zassert_false(akira_display_clip_rect(0, 240, 10, 10, XRES, YRES,
                                          &x1, &y1, &x2, &y2),
                  "rect starting past the bottom edge must be fully clipped");
    zassert_false(akira_display_clip_rect(0, -100, 10, 50, XRES, YRES,
                                          &x1, &y1, &x2, &y2),
                  "rect ending above the top edge must be fully clipped");
}

/* x + w / y + h overflow signed int. Signed overflow is UB, so the compiler may
 * assume it never happens and delete a naive bounds check outright. */
ZTEST(display_clip, test_reject_coordinate_overflow)
{
    int x1, y1, x2, y2;

    zassert_false(akira_display_clip_rect(INT32_MAX, 0, 1, 1, XRES, YRES,
                                          &x1, &y1, &x2, &y2),
                  "x + w overflow must not wrap into a valid-looking rect");
    zassert_false(akira_display_clip_rect(0, INT32_MAX, 1, 1, XRES, YRES,
                                          &x1, &y1, &x2, &y2),
                  "y + h overflow must not wrap into a valid-looking rect");
    zassert_false(akira_display_clip_rect(INT32_MIN, 0, 1, 1, XRES, YRES,
                                          &x1, &y1, &x2, &y2),
                  "INT32_MIN origin must be fully clipped");
}

ZTEST(display_clip, test_reject_degenerate_dimensions)
{
    int x1, y1, x2, y2;

    zassert_false(akira_display_clip_rect(0, 0, 0, 10, XRES, YRES,
                                          &x1, &y1, &x2, &y2), "w = 0");
    zassert_false(akira_display_clip_rect(0, 0, 10, 0, XRES, YRES,
                                          &x1, &y1, &x2, &y2), "h = 0");
    zassert_false(akira_display_clip_rect(0, 0, -1, 10, XRES, YRES,
                                          &x1, &y1, &x2, &y2), "w < 0");
    zassert_false(akira_display_clip_rect(0, 0, 10, -1, XRES, YRES,
                                          &x1, &y1, &x2, &y2), "h < 0");
    zassert_false(akira_display_clip_rect(0, 0, 10, 10, 0, 0,
                                          &x1, &y1, &x2, &y2), "zero panel");
}

/* ------------------------------------------------------------------ */
/* Accepted — must still clip rather than reject                        */
/* ------------------------------------------------------------------ */

ZTEST(display_clip, test_fully_on_screen_is_unchanged)
{
    int x1, y1, x2, y2;

    zassert_true(akira_display_clip_rect(10, 10, 50, 50, XRES, YRES,
                                         &x1, &y1, &x2, &y2), NULL);
    zassert_equal(x1, 10, "x1");
    zassert_equal(y1, 10, "y1");
    zassert_equal(x2, 60, "x2 is exclusive: 10 + 50");
    zassert_equal(y2, 60, "y2 is exclusive: 10 + 50");
}

/* The case the originally-suggested fix ("reject x < 0") would have broken.
 * Drawing a sprite that hangs off the left edge is legitimate. */
ZTEST(display_clip, test_straddling_left_edge_is_clipped_not_rejected)
{
    int x1, y1, x2, y2;

    zassert_true(akira_display_clip_rect(-10, 20, 50, 30, XRES, YRES,
                                         &x1, &y1, &x2, &y2),
                 "partially visible rect must still be drawn");
    zassert_equal(x1, 0,  "clipped to the left edge");
    zassert_equal(x2, 40, "-10 + 50 = 40 visible columns");
    zassert_equal(y1, 20, "y untouched");
    zassert_equal(y2, 50, NULL);
}

ZTEST(display_clip, test_straddling_top_edge_is_clipped_not_rejected)
{
    int x1, y1, x2, y2;

    zassert_true(akira_display_clip_rect(20, -10, 30, 50, XRES, YRES,
                                         &x1, &y1, &x2, &y2), NULL);
    zassert_equal(y1, 0,  "clipped to the top edge");
    zassert_equal(y2, 40, "-10 + 50 = 40 visible rows");
}

ZTEST(display_clip, test_straddling_far_edges_is_clamped_to_panel)
{
    int x1, y1, x2, y2;

    zassert_true(akira_display_clip_rect(300, 220, 100, 100, XRES, YRES,
                                         &x1, &y1, &x2, &y2), NULL);
    zassert_equal(x1, 300,  NULL);
    zassert_equal(y1, 220,  NULL);
    zassert_equal(x2, XRES, "clamped to panel width");
    zassert_equal(y2, YRES, "clamped to panel height");
}

/* A rect larger than the panel in every direction still yields the full panel,
 * not an empty or inverted result. */
ZTEST(display_clip, test_covering_whole_panel)
{
    int x1, y1, x2, y2;

    zassert_true(akira_display_clip_rect(-1000, -1000, 5000, 5000, XRES, YRES,
                                         &x1, &y1, &x2, &y2), NULL);
    zassert_equal(x1, 0,    NULL);
    zassert_equal(y1, 0,    NULL);
    zassert_equal(x2, XRES, NULL);
    zassert_equal(y2, YRES, NULL);
}

/* Exact-edge boundaries: the last valid pixel must be accepted and the first
 * invalid one rejected. Off-by-one here is what the whole fix turns on. */
ZTEST(display_clip, test_exact_edge_boundaries)
{
    int x1, y1, x2, y2;

    zassert_true(akira_display_clip_rect(XRES - 1, YRES - 1, 1, 1, XRES, YRES,
                                         &x1, &y1, &x2, &y2),
                 "last on-screen pixel must be drawable");
    zassert_equal(x2, XRES, NULL);
    zassert_equal(y2, YRES, NULL);

    zassert_false(akira_display_clip_rect(XRES, 0, 1, 1, XRES, YRES,
                                          &x1, &y1, &x2, &y2),
                  "first column past the panel must be rejected");
    zassert_false(akira_display_clip_rect(0, YRES, 1, 1, XRES, YRES,
                                          &x1, &y1, &x2, &y2),
                  "first row past the panel must be rejected");
}

/* The helper must not touch the outputs when it returns false — callers are
 * entitled to leave them uninitialised. */
ZTEST(display_clip, test_outputs_untouched_when_fully_clipped)
{
    int x1 = -12345, y1 = -12345, x2 = -12345, y2 = -12345;

    zassert_false(akira_display_clip_rect(400, 0, 10, 10, XRES, YRES,
                                          &x1, &y1, &x2, &y2), NULL);
    zassert_equal(x1, -12345, "x1 must not be written on the reject path");
    zassert_equal(y1, -12345, "y1 must not be written on the reject path");
    zassert_equal(x2, -12345, "x2 must not be written on the reject path");
    zassert_equal(y2, -12345, "y2 must not be written on the reject path");
}


/* ------------------------------------------------------------------ */
/* Line and 64-bit box clipping                                         */
/* ------------------------------------------------------------------ */

/* Bresenham walks every point of the segment it is given: clipping first is
 * what bounds display_line() to the panel instead of ~2^32 steps. */
ZTEST(display_clip, test_line_on_screen_is_unchanged)
{
    int x0, y0, x1, y1;

    zassert_true(akira_display_clip_line(10, 20, 300, 200, XRES, YRES,
                                         &x0, &y0, &x1, &y1), NULL);
    zassert_equal(x0, 10, NULL);
    zassert_equal(y0, 20, NULL);
    zassert_equal(x1, 300, NULL);
    zassert_equal(y1, 200, NULL);
}

ZTEST(display_clip, test_line_extremes_end_on_panel)
{
    int x0, y0, x1, y1;

    zassert_true(akira_display_clip_line(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX,
                                         XRES, YRES, &x0, &y0, &x1, &y1), NULL);
    zassert_true(x0 >= 0 && x0 < XRES && x1 >= 0 && x1 < XRES, "x on panel");
    zassert_true(y0 >= 0 && y0 < YRES && y1 >= 0 && y1 < YRES, "y on panel");

    zassert_true(akira_display_clip_line(-1000, 120, 1000, 120, XRES, YRES,
                                         &x0, &y0, &x1, &y1), NULL);
    zassert_equal(x0, 0, "clipped to the left edge");
    zassert_equal(x1, XRES - 1, "end points are inclusive");
    zassert_equal(y0, 120, NULL);
    zassert_equal(y1, 120, NULL);
}

ZTEST(display_clip, test_line_fully_off_screen_rejected)
{
    int x0 = -7, y0 = -7, x1 = -7, y1 = -7;

    zassert_false(akira_display_clip_line(-10, -10, -5, -1, XRES, YRES,
                                          &x0, &y0, &x1, &y1), NULL);
    zassert_false(akira_display_clip_line(XRES, 0, INT32_MAX, YRES - 1, XRES, YRES,
                                          &x0, &y0, &x1, &y1), NULL);
    zassert_equal(x0, -7, "outputs untouched on reject");
    zassert_equal(y1, -7, "outputs untouched on reject");
}

/* Derived edges (x + w - radius, ...) exceed int; the box clip takes them
 * as int64_t. */
ZTEST(display_clip, test_box_takes_64bit_edges)
{
    int x0, y0, x1, y1;

    zassert_true(akira_display_clip_box((int64_t)INT32_MIN - 5, -1,
                                        (int64_t)INT32_MAX + 5, 1,
                                        XRES, YRES, &x0, &y0, &x1, &y1), NULL);
    zassert_equal(x0, 0, NULL);
    zassert_equal(y0, 0, NULL);
    zassert_equal(x1, XRES, NULL);
    zassert_equal(y1, 1, NULL);
    zassert_false(akira_display_clip_box(10, 10, 10, 20, XRES, YRES,
                                         &x0, &y0, &x1, &y1), "empty box");
}
