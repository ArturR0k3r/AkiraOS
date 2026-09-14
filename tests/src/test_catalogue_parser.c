/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <zephyr/ztest.h>
#include <string.h>
#include "catalogue_parser.h"

ZTEST_SUITE(catalogue_parser, NULL, NULL, NULL, NULL, NULL);

ZTEST(catalogue_parser, test_parses_two_apps)
{
    const char *json =
        "{"
        "\"generated_at\":\"2026-09-10T00:00:00Z\","
        "\"tag\":\"v1.2.3\","
        "\"apps\":["
        "{\"id\":\"console_apps/badge\",\"name\":\"badge\",\"display_name\":\"Akira Badge\","
        "\"category\":\"console_apps\",\"version\":\"1.1.0\",\"size_bytes\":4096,"
        "\"capabilities\":[\"display.write\"],\"min_akiraos_version\":\"1.5.0\","
        "\"download_url\":\"https://github.com/ArturR0k3r/AkiraSDK/releases/download/v1.2.3/console_apps-badge.akpkg\","
        "\"icon_url\":null},"
        "{\"id\":\"console_apps/level_tool\",\"name\":\"level_tool\",\"display_name\":\"Level Tool\","
        "\"category\":\"console_apps\",\"version\":\"1.0.0\",\"size_bytes\":2048,"
        "\"capabilities\":[],\"min_akiraos_version\":\"1.5.0\","
        "\"download_url\":\"https://github.com/ArturR0k3r/AkiraSDK/releases/download/v1.2.3/console_apps-level_tool.akpkg\","
        "\"icon_url\":null}"
        "]}";

    catalogue_entry_t entries[8];
    int n = catalogue_parse(json, strlen(json), entries, 8);

    zassert_equal(n, 2, "should parse both apps");
    zassert_str_equal(entries[0].name, "badge", "first app name");
    zassert_str_equal(entries[0].version, "1.1.0", "first app version");
    zassert_str_equal(entries[0].download_url,
        "https://github.com/ArturR0k3r/AkiraSDK/releases/download/v1.2.3/console_apps-badge.akpkg",
        "first app download_url");
    zassert_str_equal(entries[1].name, "level_tool", "second app name");
}

ZTEST(catalogue_parser, test_empty_apps_array)
{
    const char *json = "{\"generated_at\":\"x\",\"tag\":\"v1\",\"apps\":[]}";
    catalogue_entry_t entries[8];
    int n = catalogue_parse(json, strlen(json), entries, 8);
    zassert_equal(n, 0, "empty apps array parses to zero entries");
}

ZTEST(catalogue_parser, test_missing_apps_key_is_error)
{
    const char *json = "{\"generated_at\":\"x\",\"tag\":\"v1\"}";
    catalogue_entry_t entries[8];
    int n = catalogue_parse(json, strlen(json), entries, 8);
    zassert_true(n < 0, "missing apps array must return negative errno");
}

ZTEST(catalogue_parser, test_caps_at_max_entries)
{
    /* 3 apps in the JSON, but caller only has room for 2 */
    const char *json =
        "{\"apps\":["
        "{\"name\":\"a\",\"version\":\"1.0.0\",\"download_url\":\"http://x/a\"},"
        "{\"name\":\"b\",\"version\":\"1.0.0\",\"download_url\":\"http://x/b\"},"
        "{\"name\":\"c\",\"version\":\"1.0.0\",\"download_url\":\"http://x/c\"}"
        "]}";
    catalogue_entry_t entries[2];
    int n = catalogue_parse(json, strlen(json), entries, 2);
    zassert_equal(n, 2, "must cap at max_entries, not overflow the array");
}

ZTEST(catalogue_parser, test_malformed_entry_is_skipped_not_fatal)
{
    /* second entry has no closing brace before the array closes -> skip it, keep first and third */
    const char *json =
        "{\"apps\":["
        "{\"name\":\"a\",\"version\":\"1.0.0\",\"download_url\":\"http://x/a\"},"
        "{\"name\":\"b\",\"version\":\"1.0.0\","
        "{\"name\":\"c\",\"version\":\"1.0.0\",\"download_url\":\"http://x/c\"}"
        "]}";
    catalogue_entry_t entries[8];
    int n = catalogue_parse(json, strlen(json), entries, 8);
    zassert_true(n >= 1, "at least the well-formed first entry must parse");
    zassert_str_equal(entries[0].name, "a", "first entry still parses despite a later malformed one");
}

ZTEST(catalogue_parser, test_parses_tags_and_thumbnail)
{
    const char *json =
        "{\"apps\":["
        "{\"name\":\"tetris\",\"version\":\"1.0.0\",\"download_url\":\"http://x/t\","
        "\"tags\":[\"NES\",\"PUZZLE\",\"UNKNOWN_TAG\"],"
        "\"thumbnail_url\":\"https://console.app.akiraos.dev/catalog/thumbnails/retro-tetris.bin\"}"
        "]}";
    catalogue_entry_t entries[8];
    int n = catalogue_parse(json, strlen(json), entries, 8);

    zassert_equal(n, 1, "single app parses");
    zassert_equal(entries[0].tag_mask, CATALOGUE_TAG_NES | CATALOGUE_TAG_PUZZLE,
        "unrecognized tag is ignored, recognized tags set their bit");
    zassert_str_equal(entries[0].thumbnail_url,
        "https://console.app.akiraos.dev/catalog/thumbnails/retro-tetris.bin",
        "thumbnail_url parsed");
}

ZTEST(catalogue_parser, test_missing_tags_and_thumbnail_default_empty)
{
    const char *json =
        "{\"apps\":["
        "{\"name\":\"a\",\"version\":\"1.0.0\",\"download_url\":\"http://x/a\"}"
        "]}";
    catalogue_entry_t entries[8];
    int n = catalogue_parse(json, strlen(json), entries, 8);

    zassert_equal(n, 1, "single app parses");
    zassert_equal(entries[0].tag_mask, 0, "no tags key -> empty mask");
    zassert_str_equal(entries[0].thumbnail_url, "", "no thumbnail_url key -> empty string");
}
