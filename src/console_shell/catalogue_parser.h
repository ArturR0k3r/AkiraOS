/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef CATALOGUE_PARSER_H
#define CATALOGUE_PARSER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CATALOGUE_MAX_APPS 32
#define CATALOGUE_NAME_LEN 40
#define CATALOGUE_VERSION_LEN 16
#define CATALOGUE_URL_LEN 160
#define CATALOGUE_CATEGORY_LEN 24
#define CATALOGUE_DISPLAY_NAME_LEN 40

/* Fixed tag taxonomy — bit position == index into catalogue_tag_names().
 * Keep in sync with AkiraConsoleApp's src/lib/tags.ts and
 * functions/api/submit.ts, which validate submissions against this same
 * ordered list. Stored as a bitmask rather than strings: fixed set, cheap
 * to filter/match on-device, no string storage per entry. */
#define CATALOGUE_TAG_NES        (1u << 0)
#define CATALOGUE_TAG_SNES       (1u << 1)
#define CATALOGUE_TAG_SMS        (1u << 2)
#define CATALOGUE_TAG_GENESIS    (1u << 3)
#define CATALOGUE_TAG_GB         (1u << 4)
#define CATALOGUE_TAG_GBA        (1u << 5)
#define CATALOGUE_TAG_ARCADE     (1u << 6)
#define CATALOGUE_TAG_RPG        (1u << 7)
#define CATALOGUE_TAG_PLATFORMER (1u << 8)
#define CATALOGUE_TAG_PUZZLE     (1u << 9)
#define CATALOGUE_TAG_DUNGEON    (1u << 10)
#define CATALOGUE_TAG_SHOOTER    (1u << 11)
#define CATALOGUE_TAG_ADVENTURE  (1u << 12)
#define CATALOGUE_TAG_STRATEGY   (1u << 13)
#define CATALOGUE_TAG_UTILITY    (1u << 14)
#define CATALOGUE_TAG_COUNT 15

/** Name for tag bit index @p i (0-based, < CATALOGUE_TAG_COUNT). */
const char *catalogue_tag_name(int i);

typedef struct {
    char name[CATALOGUE_NAME_LEN];
    char version[CATALOGUE_VERSION_LEN];
    char download_url[CATALOGUE_URL_LEN];
    /* Optional fields — default to name/"generic"/0 when absent from the
     * JSON so older catalogues (name+version+download_url only) still parse. */
    char category[CATALOGUE_CATEGORY_LEN];
    char display_name[CATALOGUE_DISPLAY_NAME_LEN];
    uint32_t size_bytes;
    uint32_t tag_mask;
    char thumbnail_url[CATALOGUE_URL_LEN];
} catalogue_entry_t;

/** Parse a catalogue.json buffer into a caller-owned array.
 *  @param json       Raw JSON bytes (not necessarily null-terminated).
 *  @param json_len   Length of @p json.
 *  @param out        Caller-owned array of at least @p max_entries elements.
 *  @param max_entries Capacity of @p out.
 *  @return Number of entries parsed (>=0, capped at max_entries), or
 *          negative errno on malformed top-level JSON (missing "apps" array
 *          or invalid syntax before any entry could be parsed).
 *          A malformed *individual* entry is skipped, not fatal — matches
 *          gen_catalogue.py's own per-app error isolation.
 */
int catalogue_parse(const char *json, size_t json_len,
                     catalogue_entry_t *out, int max_entries);

#ifdef __cplusplus
}
#endif

#endif /* CATALOGUE_PARSER_H */
