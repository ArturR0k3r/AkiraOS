/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file list_screen.h
 * @brief Declarative settings-style list, shared by every settings page.
 *
 * A page is a const list_model_t: a title and a table of list_item_t rows.
 * The list screen draws them as dither-shadow cards with a highlight that
 * slides between rows, scrolls smoothly, and staggers the rows in when the
 * page opens.  Keys: UP/DOWN select, A activates, LEFT/RIGHT adjust
 * toggles/choices/steppers, B goes back.
 *
 * Values are read through get() on every repaint, so a model should cache
 * anything expensive (NVS reads) in on_enter() and return the cached copy.
 */

#ifndef AKIRA_SHELL_LIST_SCREEN_H
#define AKIRA_SHELL_LIST_SCREEN_H

#include <stdbool.h>
#include <stddef.h>

#include "screen.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LI_NAV,     /**< A opens `screen` (pushed with `arg`) */
    LI_ACTION,  /**< A runs action() */
    LI_TOGGLE,  /**< A / LEFT / RIGHT flip get()/set() between 0 and 1 */
    LI_CHOICE,  /**< A / LEFT / RIGHT cycle through choices[0..n_choices) */
    LI_STEPPER, /**< LEFT / RIGHT add -step / +step within [min, max] */
    LI_INFO,    /**< read-only label + value() */
} list_kind_t;

struct list_item;

typedef struct list_item {
    list_kind_t kind;
    const char *label;

    const shell_screen_t *screen;   /**< LI_NAV */
    void *arg;                      /**< LI_NAV push arg; free for callbacks */

    void (*action)(const struct list_item *it);         /**< LI_ACTION */
    int (*get)(const struct list_item *it);             /**< TOGGLE/CHOICE/STEPPER */
    void (*set)(const struct list_item *it, int value); /**< TOGGLE/CHOICE/STEPPER */

    int min, max, step;             /**< LI_STEPPER */
    const char *unit;               /**< LI_STEPPER suffix, e.g. "%" */
    const char *const *choices;     /**< LI_CHOICE labels */
    int n_choices;

    /** Right-hand text (LI_INFO, or overrides the default for any kind). */
    void (*value)(const struct list_item *it, char *buf, size_t len);
    /** Optional: false greys the row out and ignores changes. */
    bool (*enabled)(const struct list_item *it);
} list_item_t;

typedef struct list_model {
    const char *title;
    const list_item_t *items;
    int count;
    void (*on_enter)(void);  /**< load / cache values */
    void (*on_leave)(void);
    /** Optional extra content drawn under the rows (e.g. a big battery). */
    void (*draw_extra)(int y_after_rows);
    bool live;               /**< repaint values every second */
    const char *hint_l;      /**< footer override (default "A:Select") */
    const char *hint_r;      /**< footer override (default "B:Back") */
} list_model_t;

/** The generic list screen.  Push it with the model as arg. */
extern const shell_screen_t list_screen;

/** screen_push(&list_screen, model) */
void list_screen_open(const list_model_t *model);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SHELL_LIST_SCREEN_H */
