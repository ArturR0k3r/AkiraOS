/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_display_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_display_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file display_screen.c
 * @brief Settings → Display: brightness and screen-off (wait screen) timeout.
 *
 * The shell main loop re-reads akira/display/timeout_* whenever the settings
 * generation changes, so edits here apply within a second.
 */

#include <zephyr/kernel.h>
#include <settings/settings.h>
#include <stdlib.h>
#include <stdio.h>

#include <drivers/platform_hal.h>
#include "display_screen.h"

#define BRIGHT_MIN 10
#define BRIGHT_MAX 100
#define BRIGHT_STEP 10

#define TMOUT_MIN 5
#define TMOUT_MAX 300
#define TMOUT_STEP 5

static int g_brightness = BRIGHT_MAX;
static int g_timeout = 60;
static bool g_timeout_en = true;

static int read_int(const char *key, int def)
{
    char sv[16] = "";

    return akira_settings_get(key, sv, sizeof(sv)) == 0 ? atoi(sv) : def;
}

static void write_int(const char *key, int v)
{
    char sv[16];

    snprintf(sv, sizeof(sv), "%d", v);
    akira_settings_set(key, sv, 0);
}

static void apply_brightness(int v)
{
    uint8_t hw = (uint8_t)((v * 255 + 50) / 100);
    akira_display_hal_set_brightness(hw);
    LOG_DBG("Bright=%d -> hw=%u", v, hw);
}

static void display_enter(void)
{
    g_brightness = CLAMP(read_int("akira/display/brightness", BRIGHT_MAX),
                         BRIGHT_MIN, BRIGHT_MAX);
    g_timeout = CLAMP(read_int("akira/display/timeout_s", 60), TMOUT_MIN, TMOUT_MAX);
    g_timeout_en = read_int("akira/display/timeout_en", 1) != 0;
}

static int get_brightness(const list_item_t *it)
{
    ARG_UNUSED(it);
    return g_brightness;
}

static void set_brightness(const list_item_t *it, int v)
{
    ARG_UNUSED(it);
    g_brightness = v;
    apply_brightness(v); /* live preview */
    write_int("akira/display/brightness", v);
}

static int get_timeout_en(const list_item_t *it)
{
    ARG_UNUSED(it);
    return g_timeout_en;
}

static void set_timeout_en(const list_item_t *it, int v)
{
    ARG_UNUSED(it);
    g_timeout_en = v;
    akira_settings_set("akira/display/timeout_en", v ? "1" : "0", 0);
}

static int get_timeout(const list_item_t *it)
{
    ARG_UNUSED(it);
    return g_timeout;
}

static void set_timeout(const list_item_t *it, int v)
{
    ARG_UNUSED(it);
    g_timeout = v;
    write_int("akira/display/timeout_s", v);
}

static bool timeout_enabled(const list_item_t *it)
{
    ARG_UNUSED(it);
    return g_timeout_en;
}

static const list_item_t g_items[] = {
    { .kind = LI_STEPPER, .label = "Brightness", .get = get_brightness,
      .set = set_brightness, .min = BRIGHT_MIN, .max = BRIGHT_MAX,
      .step = BRIGHT_STEP, .unit = "%" },
    { .kind = LI_TOGGLE, .label = "Auto screen off", .get = get_timeout_en,
      .set = set_timeout_en },
    { .kind = LI_STEPPER, .label = "Screen off after", .get = get_timeout,
      .set = set_timeout, .min = TMOUT_MIN, .max = TMOUT_MAX, .step = TMOUT_STEP,
      .unit = "s", .enabled = timeout_enabled },
};

const list_model_t display_page = {
    .title = "Display",
    .items = g_items,
    .count = ARRAY_SIZE(g_items),
    .on_enter = display_enter,
};
