/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_SHELL_BOOT_SCREEN_H
#define AKIRA_SHELL_BOOT_SCREEN_H

#include "ui/screen.h"

/**
 * Animated boot splash: the AkiraOS crystal grows out of its core, a glint
 * crosses it, the wordmark rises in, and the scene washes out to the shell's
 * background while the home cards fan out.  ~3 s; any key skips to the end.
 *
 * Pushed over the home screen once, at shell start.  Runs while main() is
 * still bringing the system up, so it costs no boot time of its own.
 */
extern const shell_screen_t boot_screen;

#endif /* AKIRA_SHELL_BOOT_SCREEN_H */
