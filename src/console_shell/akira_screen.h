/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_SHELL_AKIRA_SCREEN_H
#define AKIRA_SHELL_AKIRA_SCREEN_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Settings → Akira & Seals: the crystal, turning, what grew it in plain
 * words, and the Seal history, always-allowed powers (revocable) and cracks
 * (mend, or remove the app).
 */
void akira_screen_open(void);

/** "How it works": four pages, one with a practice Seal. */
void akira_intro_open(bool first_run);

/** Once per boot, from the home screen: show the intro if never seen. */
void akira_intro_first_run_check(void);

/** One-word state for the Settings row ("Day 40", "1 crack"). */
void akira_screen_status(char *buf, size_t len);

#endif /* AKIRA_SHELL_AKIRA_SCREEN_H */
