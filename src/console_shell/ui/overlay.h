/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file overlay.h
 * @brief Transient layers drawn over whatever screen is up.
 *
 *   - toasts: a card that drops in under the status bar, holds, and leaves;
 *     queued, so a burst of events is shown one after another;
 *   - install progress: a bottom card fed by install events, auto-hiding;
 *   - charging splash: the large animated battery shown on plug-in.
 *
 * All of it is non-blocking and painted by the screen framework (screen.c),
 * which calls overlay_tick() / overlay_draw() every frame.
 */

#ifndef AKIRA_SHELL_OVERLAY_H
#define AKIRA_SHELL_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "akira_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOAST_ICON_NONE = 0,
    TOAST_ICON_INFO,
    TOAST_ICON_OK,
    TOAST_ICON_ERROR,
    TOAST_ICON_SD,
    TOAST_ICON_BATTERY, /* live battery widget from the status model */
} toast_icon_t;

/** Duration for a toast that stays until overlay_toast_dismiss(). */
#define TOAST_STICKY 0

/**
 * Queue a toast.  @p line2 may be NULL.  Strings are copied.
 * @param duration_ms hold time, or TOAST_STICKY.
 * @return a handle for overlay_toast_dismiss().
 */
int overlay_toast(toast_icon_t icon, const char *line1, const char *line2,
                  int duration_ms);

/** Dismiss toast @p handle (shown or queued).  Unknown handles are ignored. */
void overlay_toast_dismiss(int handle);

/** Append animated "..." to toast @p handle's second line (a loading hint). */
void overlay_toast_set_busy(int handle, bool busy);

/**
 * Show or update the install progress card.
 * @param name app name (may be NULL to keep the previous one)
 * @param pct  0..100
 * @param msg  status line (may be NULL)
 */
void overlay_progress(const char *name, int pct, const char *msg);

/** Finish the progress card: show @p msg as the outcome, then hide. */
void overlay_progress_finish(bool ok, const char *msg);

/** True while the progress card is visible. */
bool overlay_progress_active(void);

/** Show the charging splash for a few seconds. */
void overlay_charging_splash(void);

/* ---- framework hooks (screen.c) ------------------------------------- */

/** @return true if the key was consumed (splash dismissal). */
bool overlay_handle_key(uint32_t just);

/** Advance timers/animations.  @return true if a full redraw is needed. */
bool overlay_tick(void);

/** Paint every visible overlay over the current frame. */
void overlay_draw(void);

/** True while something is moving (shell keeps its fast tick). */
bool overlay_is_animating(void);

/** Drop everything (e.g. display handed to a WASM app). */
void overlay_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SHELL_OVERLAY_H */
