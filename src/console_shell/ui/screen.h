/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file screen.h
 * @brief OS shell screen stack — every shell screen is a shell_screen_t.
 *
 * One thread (the shell thread) owns the display.  Screens never loop or
 * sleep: they react to keys in on_key(), advance animations in tick(), and
 * paint their content in draw().  The framework owns everything around that:
 *
 *   - the status bar (title + Wi-Fi/BT + battery with charging animation +
 *     clock) on every screen that sets SCR_F_STATUSBAR;
 *   - the footer hint bar (SCR_F_FOOTER);
 *   - overlays (toasts, install progress, charging splash — see overlay.h);
 *   - frame scheduling: at most one flush per shell tick, only when something
 *     changed, and a cheap "status bar only" repaint for clock / battery
 *     updates that leaves the rest of the framebuffer untouched.
 *
 * Because no screen blocks, the shell's 1 Hz housekeeping, idle timeout,
 * HOME long-press and event queue keep running whatever is on screen.
 */

#ifndef AKIRA_SHELL_SCREEN_H
#define AKIRA_SHELL_SCREEN_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Framework draws the status bar (title from the screen / screen_set_title). */
#define SCR_F_STATUSBAR BIT(0)
/** Framework draws the footer hint bar (hint_l / hint_r, or screen_set_hints). */
#define SCR_F_FOOTER    BIT(1)
/** Drawn over the screen below it, which keeps its chrome but gets no keys. */
#define SCR_F_MODAL     BIT(2)
/** screen_pop_to_root() stops at this screen (e.g. the USB-storage lock). */
#define SCR_F_STICKY    BIT(3)

/** Content area for screens with status bar + footer. */
#define SCREEN_CONTENT_Y  30
#define SCREEN_CONTENT_B  212 /* exclusive bottom when SCR_F_FOOTER */

typedef struct shell_screen {
    const char *name;     /**< for logs */
    const char *title;    /**< status-bar title; NULL = none */
    uint32_t flags;       /**< SCR_F_* */
    const char *hint_l;   /**< footer left,  e.g. "A:Select" (key:label) */
    const char *hint_r;   /**< footer right, e.g. "B:Back" */

    /** Pushed.  @p arg is whatever screen_push() was given. */
    void (*enter)(void *arg);
    /** Popped (or popped past by screen_pop_to_root()). Release resources. */
    void (*leave)(void);
    /** A screen pushed on top of this one was popped. */
    void (*resume)(void);
    /** Rising-edge button bits (BIT(AKIRA_BTN_*)). */
    void (*on_key)(uint32_t just);
    /** Every shell tick (~30 Hz) while on top. Return true to request a redraw. */
    bool (*tick)(void);
    /** Paint the content area. Never flush — the framework does. */
    void (*draw)(void);
    /** Once a second while on top. */
    void (*on_status)(void);
} shell_screen_t;

/** Install @p root as the bottom of the stack (the home screen). */
void screen_init(const shell_screen_t *root);

/** Push @p s on top, calling its enter(@p arg). */
void screen_push(const shell_screen_t *s, void *arg);

/** Pop the top screen (never the root). */
void screen_pop(void);

/** Pop the top screen and push @p s in its place. */
void screen_replace(const shell_screen_t *s, void *arg);

/** Pop everything above the root, or down to a SCR_F_STICKY screen. */
void screen_pop_to_root(void);

/** Pop @p s if it is anywhere on the stack (and everything above it). */
void screen_close(const shell_screen_t *s);

bool screen_is_root(void);
bool screen_is_open(const shell_screen_t *s);
const shell_screen_t *screen_top(void);
/** 0 for the root, 1 for the first pushed screen, ... */
int screen_depth(void);

/**
 * Stack index of the screen whose callback is running right now.  Lets one
 * shell_screen_t (e.g. the generic list screen) be stacked several times and
 * still find its own instance state — including from draw(), which also runs
 * for the screen underneath a modal.
 */
int screen_self(void);

/** Override the top screen's title (string must outlive the screen). */
void screen_set_title(const char *title);
/** Override the top screen's footer hints (strings must outlive the screen). */
void screen_set_hints(const char *hint_l, const char *hint_r);

/** Request a full redraw on the next frame. */
void screen_invalidate(void);
/** Request a status-bar-only repaint on the next frame. */
void screen_invalidate_status(void);

/* ---- driven by the shell thread (akira_os_shell.c) ------------------ */

/** Route rising-edge keys to overlays first, then the top screen. */
void screen_dispatch_key(uint32_t just);

/** One frame: tick the top screen + overlays, repaint and flush if needed. */
void screen_frame(void);

/** 1 Hz: refresh clock / connectivity, call the top screen's on_status(). */
void screen_status_tick(void);

/**
 * Stop drawing (display owned by a WASM app, or the wait screen is up).
 * Resuming forces a full redraw.
 */
void screen_set_suspended(bool suspended);
bool screen_is_suspended(void);

/** True while a screen or overlay is animating (shell may pace faster). */
bool screen_is_animating(void);

/**
 * Run a synchronous, bounded job (e.g. a TLS request) on the shell thread.
 * Paints and flushes a "busy" card with @p label first so the user sees why
 * the UI paused.  Use only where no worker thread can be afforded.
 */
void screen_run_busy(const char *label, void (*fn)(void *), void *arg);

/* ---- chrome helpers for screens that draw their own ------------------ */

/** Paint the status bar now with @p title (used by the wait screen etc.). */
void screen_draw_status_bar(const char *title);

/** Draw the dimming scrim used behind modals over the current framebuffer. */
void screen_draw_scrim(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SHELL_SCREEN_H */
