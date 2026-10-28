/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file anim.c
 * @brief Time-based integer tweens (see anim.h).
 *
 * Fixed point throughout: progress t is 0..256, curves return 0..256 (the
 * back-ease briefly exceeds 256).  No floats — this runs every frame.
 */

#include "anim.h"

#include <zephyr/kernel.h>

static int ease_curve(anim_ease_t ease, int t)
{
    switch (ease) {
    case ANIM_EASE_INOUT: {
        /* smoothstep: 3t^2 - 2t^3 */
        int t2 = t * t / 256;
        int t3 = t2 * t / 256;
        return 3 * t2 - 2 * t3;
    }
    case ANIM_EASE_OUT: {
        /* 1 - (1-t)^3 */
        int u = 256 - t;
        return 256 - (u * u / 256) * u / 256;
    }
    case ANIM_EASE_OUT_BACK: {
        /* 1 + c3 (t-1)^3 + c1 (t-1)^2, c1 = 1.70158, c3 = c1 + 1 */
        int u = t - 256;
        int u2 = u * u / 256;
        int u3 = u2 * u / 256;
        return 256 + (692 * u3) / 256 + (436 * u2) / 256;
    }
    case ANIM_LINEAR:
    default:
        return t;
    }
}

int anim_ease(anim_ease_t ease, int64_t elapsed_ms, int dur_ms)
{
    if (dur_ms <= 0 || elapsed_ms >= dur_ms) {
        return 256;
    }
    if (elapsed_ms <= 0) {
        return 0;
    }
    return ease_curve(ease, (int)(elapsed_ms * 256 / dur_ms));
}

void anim_start(anim_t *a, int from, int to, int dur_ms, anim_ease_t ease)
{
    a->from = from;
    a->to = to;
    a->t0 = k_uptime_get();
    a->dur_ms = (uint16_t)(dur_ms > 0 ? dur_ms : 0);
    a->ease = (uint8_t)ease;
}

void anim_to(anim_t *a, int to, int dur_ms, anim_ease_t ease)
{
    anim_start(a, anim_value(a), to, dur_ms, ease);
}

void anim_set(anim_t *a, int value)
{
    a->from = value;
    a->to = value;
    a->t0 = 0;
    a->dur_ms = 0;
    a->ease = ANIM_LINEAR;
}

int anim_value(const anim_t *a)
{
    if (a->dur_ms == 0) {
        return a->to;
    }
    int p = anim_ease((anim_ease_t)a->ease, k_uptime_get() - a->t0, a->dur_ms);

    return a->from + (int)(((int64_t)(a->to - a->from) * p) / 256);
}

bool anim_running(const anim_t *a)
{
    return a->dur_ms != 0 && (k_uptime_get() - a->t0) < a->dur_ms;
}
