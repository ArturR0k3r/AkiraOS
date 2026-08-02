/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef WAIT_SCREEN_H
#define WAIT_SCREEN_H

#include <stdint.h>

/**
 * @file wait_screen.h
 * @brief Idle wait / power-save screensaver for AkiraConsole.
 *
 * Called by akira_os_shell.c when the console has been idle for
 * the configured idle timeout. Shows a live clock, battery level,
 * and dims the backlight. Hold HOME to exit back to home.
 */

/** Enter wait screen: dim display, enable low-power mode, draw first frame. */
void wait_screen_enter(void);

/** Refresh the clock and battery reading; only repaints on minute rollover. */
void wait_screen_update(void);

/**
 * Upper bound on the blanked-state wake interval, in ms.
 *
 * The wait screen itself only needs a repaint once a minute, but the shell loop
 * also uses this tick to notice a USB/BLE host session appearing and to run the
 * deep-sleep countdown, so it is capped well below a minute.  (An event-driven
 * wake from the USB/BT connect callbacks would let this go to a full minute —
 * worth doing if the blanked-tier measurement says these wakes still matter.)
 */
#define WAIT_SCREEN_UPDATE_MAX_MS 10000u

/**
 * Milliseconds until the wait screen's next required repaint (the next
 * wall-clock minute boundary), clamped to at most WAIT_SCREEN_UPDATE_MAX_MS by
 * the caller.  Lets the blanked shell loop block for seconds at a time instead
 * of waking at 1 Hz for a frame that is identical 59 times out of 60.
 */
uint32_t wait_screen_ms_to_next_update(void);

/** Exit wait screen: restore brightness and disable low-power mode. */
void wait_screen_exit(void);

/**
 * Blank display, configure HOME GPIO as wakeup source, and enter deep sleep.
 * Only compiled when CONFIG_AKIRA_POWER_DEEP_SLEEP=y.  Does not return.
 */
void wait_screen_prepare_deep_sleep(void);

#endif /* WAIT_SCREEN_H */
