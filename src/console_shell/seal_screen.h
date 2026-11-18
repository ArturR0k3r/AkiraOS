/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_SHELL_SEAL_SCREEN_H
#define AKIRA_SHELL_SEAL_SCREEN_H

#include <stdbool.h>

#include <crystal/akira_seal.h>

/**
 * The Seal: hold A+B while the ring charges — one lap seals THIS TIME, a
 * second lap ALWAYS — or tap B to refuse.  Unanswered requests are refused
 * after CONFIG_AKIRA_SEAL_TIMEOUT_S.
 *
 * Pushes itself on the shell's screen stack.  When it has closed it calls
 * @p on_closed (the shell hands the display back to the app there), then
 * akira_seal_release() to wake whoever asked.
 */
void seal_screen_open(akira_seal_req_t *req, void (*on_closed)(akira_seal_req_t *req));

/** True while a seal is on screen. */
bool seal_screen_is_open(void);

/** Refuse and close immediately (HOME, the app going away). */
void seal_screen_cancel(void);

#endif /* AKIRA_SHELL_SEAL_SCREEN_H */
