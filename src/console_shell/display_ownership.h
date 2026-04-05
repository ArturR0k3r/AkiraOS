/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file display_ownership.h
 * @brief Display ownership arbitration between the OS shell and WASM apps.
 *
 * The OS shell (LVGL) and WASM apps share one physical display.  At any
 * given point exactly one owner writes to the framebuffer.  Callers must
 * hold the display before drawing and release it when done.
 *
 * The OS shell always owns the display at boot.  When an app is launched
 * the shell calls akira_display_release_to_wasm(); when the user presses
 * HOME the shell reclaims ownership with akira_display_claim_shell().
 *
 * WASM-facing draw primitives in akira_display_api.c check ownership and
 * return -EBUSY when the shell holds the display.  The WASM app should
 * treat -EBUSY as a signal to yield and retry later.
 */

#ifndef AKIRA_DISPLAY_OWNERSHIP_H
#define AKIRA_DISPLAY_OWNERSHIP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Display owner identifier */
typedef enum {
    DISPLAY_OWNER_SHELL = 0, /**< LVGL OS shell owns the display */
    DISPLAY_OWNER_WASM,      /**< Active WASM app owns the display */
} display_owner_t;

/**
 * @brief Claim exclusive display access for the OS shell.
 *
 * Blocks until the display is available (max 500 ms), then sets the
 * owner to DISPLAY_OWNER_SHELL and resumes the LVGL tick timer.
 *
 * Safe to call from any thread, including the shell's SYS_INIT thread.
 *
 * @return 0 on success, -ETIMEDOUT if the current owner did not yield.
 */
int akira_display_claim_shell(void);

/**
 * @brief Release the display to the active WASM app.
 *
 * Pauses the LVGL tick (stops LVGL from touching the framebuffer) and
 * sets the owner to DISPLAY_OWNER_WASM.
 *
 * The shell must call this before signalling the app to start drawing.
 */
void akira_display_release_to_wasm(void);

/**
 * @brief Query the current display owner.
 *
 * @return Current owner (DISPLAY_OWNER_SHELL or DISPLAY_OWNER_WASM).
 */
display_owner_t akira_display_get_owner(void);

/**
 * @brief Return true if the OS shell currently owns the display.
 */
static inline bool akira_display_shell_owns(void)
{
    return akira_display_get_owner() == DISPLAY_OWNER_SHELL;
}

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_DISPLAY_OWNERSHIP_H */
