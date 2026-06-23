/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef WAIT_SCREEN_H
#define WAIT_SCREEN_H

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

/** Refresh the clock and battery reading; called every 1 s while active. */
void wait_screen_update(void);

/** Exit wait screen: restore brightness and disable low-power mode. */
void wait_screen_exit(void);

/**
 * Blank display, configure HOME GPIO as wakeup source, and enter deep sleep.
 * Only compiled when CONFIG_AKIRA_POWER_DEEP_SLEEP=y.  Does not return.
 */
void wait_screen_prepare_deep_sleep(void);

#endif /* WAIT_SCREEN_H */
