/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file anim.h
 * @brief Time-based integer tweens for the OS shell.
 *
 * An anim_t interpolates an int from `from` to `to` over `dur_ms`, eased.
 * Values are sampled from k_uptime_get() whenever they're read, so motion
 * keeps its speed regardless of how often the shell manages to draw a frame.
 */

#ifndef AKIRA_SHELL_ANIM_H
#define AKIRA_SHELL_ANIM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANIM_LINEAR = 0,
    ANIM_EASE_INOUT,   /**< smoothstep — panels, carousel resize */
    ANIM_EASE_OUT,     /**< cubic out — selection, toasts, scrolling */
    ANIM_EASE_OUT_BACK,/**< overshoots then settles — "pop" */
} anim_ease_t;

typedef struct {
    int32_t from;
    int32_t to;
    int64_t t0;
    uint16_t dur_ms;
    uint8_t ease;
} anim_t;

/** Start animating from @p from to @p to. */
void anim_start(anim_t *a, int from, int to, int dur_ms, anim_ease_t ease);

/** Animate from the current value to @p to (smooth when interrupted). */
void anim_to(anim_t *a, int to, int dur_ms, anim_ease_t ease);

/** Jump to @p value with no motion. */
void anim_set(anim_t *a, int value);

/** Current value. */
int anim_value(const anim_t *a);

/** True until the tween reaches its target. */
bool anim_running(const anim_t *a);

/** Eased progress 0..256 of an arbitrary window (for staggered effects). */
int anim_ease(anim_ease_t ease, int64_t elapsed_ms, int dur_ms);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SHELL_ANIM_H */
