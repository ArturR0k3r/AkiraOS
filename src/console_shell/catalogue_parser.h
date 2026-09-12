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

typedef struct {
    char name[CATALOGUE_NAME_LEN];
    char version[CATALOGUE_VERSION_LEN];
    char download_url[CATALOGUE_URL_LEN];
    /* Optional fields — default to name/"generic"/0 when absent from the
     * JSON so older catalogues (name+version+download_url only) still parse. */
    char category[CATALOGUE_CATEGORY_LEN];
    char display_name[CATALOGUE_DISPLAY_NAME_LEN];
    uint32_t size_bytes;
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
