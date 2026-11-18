/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite: app_resources
 *
 * The release-hook registry (akira_app_containment.h): every hook registered
 * with AKIRA_APP_RESOURCE_DEFINE() runs, in its declared order, with the
 * app's slot and name, on every exit path — and running it twice is allowed.
 *
 * Sources exercised: src/runtime/app_resources.c
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <akira_app_containment.h>

#define MAX_CALLS 8

static struct {
    char id;
    int slot;
    char name[16];
} g_calls[MAX_CALLS];
static int g_ncalls;
static int g_nstops;

static void record(char id, int slot, const char *name)
{
    if (g_ncalls < MAX_CALLS) {
        g_calls[g_ncalls].id = id;
        g_calls[g_ncalls].slot = slot;
        strncpy(g_calls[g_ncalls].name, name, sizeof(g_calls[0].name) - 1);
        g_calls[g_ncalls].name[sizeof(g_calls[0].name) - 1] = '\0';
    }
    g_ncalls++;
}

static void release_late(int slot, const char *name) { record('L', slot, name); }
static void release_early(int slot, const char *name) { record('E', slot, name); }
static void release_mid(int slot, const char *name) { record('M', slot, name); }
static void stop_mid(int slot, const char *name)
{
    ARG_UNUSED(name);
    zassert_equal(slot, 3, "on_stop gets the slot");
    g_nstops++;
}

/* Declared out of order on purpose: the two-digit order decides. */
AKIRA_APP_RESOURCE_DEFINE(test_late, 90, NULL, release_late);
AKIRA_APP_RESOURCE_DEFINE(test_early, 05, NULL, release_early);
AKIRA_APP_RESOURCE_DEFINE(test_mid, 50, stop_mid, release_mid);

static void reset(void *fixture)
{
    ARG_UNUSED(fixture);
    memset(g_calls, 0, sizeof(g_calls));
    g_ncalls = 0;
    g_nstops = 0;
}

ZTEST_SUITE(app_resources, NULL, NULL, reset, NULL, NULL);

ZTEST(app_resources, test_release_runs_every_hook_in_order)
{
    akira_app_resources_release(3, "game");

    /* Other suites may register hooks too; ours must appear in order. */
    int e = -1, m = -1, l = -1;

    for (int i = 0; i < MIN(g_ncalls, MAX_CALLS); i++) {
        if (g_calls[i].id == 'E') {
            e = i;
        } else if (g_calls[i].id == 'M') {
            m = i;
        } else if (g_calls[i].id == 'L') {
            l = i;
        }
    }
    zassert_true(e >= 0 && m >= 0 && l >= 0, "every hook ran");
    zassert_true(e < m && m < l, "hooks run in declared order (E=%d M=%d L=%d)", e, m, l);
    zassert_equal(g_calls[m].slot, 3, "hook gets the slot");
    zassert_str_equal(g_calls[m].name, "game", "hook gets the app name");
}

ZTEST(app_resources, test_release_is_repeatable)
{
    akira_app_resources_release(1, "a");
    int first = g_ncalls;

    akira_app_resources_release(1, "a");
    zassert_equal(g_ncalls, 2 * first, "a second release runs every hook again");
}

ZTEST(app_resources, test_null_name_becomes_empty)
{
    akira_app_resources_release(0, NULL);
    zassert_true(g_ncalls >= 3, "hooks ran");
    zassert_str_equal(g_calls[0].name, "", "NULL name is passed as \"\"");
}

ZTEST(app_resources, test_on_stop_only_calls_stop_hooks)
{
    akira_app_resources_on_stop_request(3, "game");
    zassert_equal(g_nstops, 1, "the on_stop hook ran once");
    zassert_equal(g_ncalls, 0, "no release hook ran");
}
