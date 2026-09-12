/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "catalogue_parser.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

/* Copies the string value at *p (which must point at the opening quote) into
 * out (NUL-terminated, truncated to out_size-1). Returns pointer just past
 * the closing quote, or NULL on malformed input (no closing quote before end). */
static const char *parse_string(const char *p, const char *end, char *out, size_t out_size)
{
    if (p >= end || *p != '"') {
        return NULL;
    }
    p++;
    size_t i = 0;
    while (p < end && *p != '"') {
        if (i + 1 < out_size) {
            out[i++] = *p;
        }
        p++;
    }
    if (p >= end) {
        return NULL; /* unterminated string */
    }
    out[i] = '\0';
    return p + 1;
}

/* Finds the given key's string value within one object's bytes [obj, obj_end).
 * Returns 0 and fills out on success, -ENOENT if the key isn't found or its
 * value isn't a string. */
static int find_string_field(const char *obj, const char *obj_end,
                              const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *p = obj;

    while (p < obj_end) {
        const char *quote = memchr(p, '"', (size_t)(obj_end - p));
        if (!quote) {
            return -ENOENT;
        }
        char found_key[64];
        const char *after_key = parse_string(quote, obj_end, found_key, sizeof(found_key));
        if (!after_key) {
            return -ENOENT;
        }
        p = skip_ws(after_key, obj_end);
        if (p >= obj_end || *p != ':') {
            p = after_key;
            continue;
        }
        p = skip_ws(p + 1, obj_end);
        if (strncmp(found_key, key, key_len) == 0 && found_key[key_len] == '\0') {
            if (p < obj_end && *p == '"') {
                if (!parse_string(p, obj_end, out, out_size)) {
                    return -ENOENT;
                }
                return 0;
            }
            return -ENOENT; /* value isn't a string (e.g. null, number) */
        }
        p = after_key;
    }
    return -ENOENT;
}

/* Finds the given key's numeric value within one object's bytes [obj, obj_end).
 * Returns 0 and fills *out on success, -ENOENT if the key isn't found or its
 * value isn't a bare (non-quoted) number. */
static int find_number_field(const char *obj, const char *obj_end,
                              const char *key, uint32_t *out)
{
    size_t key_len = strlen(key);
    const char *p = obj;

    while (p < obj_end) {
        const char *quote = memchr(p, '"', (size_t)(obj_end - p));
        if (!quote) {
            return -ENOENT;
        }
        char found_key[64];
        const char *after_key = parse_string(quote, obj_end, found_key, sizeof(found_key));
        if (!after_key) {
            return -ENOENT;
        }
        p = skip_ws(after_key, obj_end);
        if (p >= obj_end || *p != ':') {
            p = after_key;
            continue;
        }
        p = skip_ws(p + 1, obj_end);
        if (strncmp(found_key, key, key_len) == 0 && found_key[key_len] == '\0') {
            if (p < obj_end && (*p == '-' || (*p >= '0' && *p <= '9'))) {
                *out = (uint32_t)strtoul(p, NULL, 10);
                return 0;
            }
            return -ENOENT; /* value isn't a bare number (e.g. string, null) */
        }
        p = after_key;
    }
    return -ENOENT;
}

/* Finds the matching closing brace for an object starting at *p (which must
 * point at '{'). Returns pointer just past the matching '}', or NULL if the
 * braces never balance before end. Ignores braces inside string values. */
static const char *skip_object(const char *p, const char *end)
{
    if (p >= end || *p != '{') {
        return NULL;
    }
    int depth = 0;
    while (p < end) {
        if (*p == '"') {
            char scratch[256];
            const char *after = parse_string(p, end, scratch, sizeof(scratch));
            if (!after) {
                return NULL;
            }
            p = after;
            continue;
        }
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) {
                return p + 1;
            }
        }
        p++;
    }
    return NULL;
}

int catalogue_parse(const char *json, size_t json_len,
                     catalogue_entry_t *out, int max_entries)
{
    const char *p = json;
    const char *end = json + json_len;

    const char *apps_key = strstr(json, "\"apps\"");
    if (!apps_key || apps_key >= end) {
        return -EINVAL;
    }
    p = apps_key + 6;
    p = skip_ws(p, end);
    if (p >= end || *p != ':') {
        return -EINVAL;
    }
    p = skip_ws(p + 1, end);
    if (p >= end || *p != '[') {
        return -EINVAL;
    }
    p++; /* past '[' */

    int count = 0;
    while (count < max_entries) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') {
            break;
        }
        if (*p != '{') {
            break; /* malformed array content */
        }

        const char *obj_start = p;
        const char *obj_end = skip_object(p, end);
        if (!obj_end) {
            break; /* unterminated object — stop, keep what parsed so far */
        }

        catalogue_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        int has_name = (find_string_field(obj_start, obj_end, "name",
                                           entry.name, sizeof(entry.name)) == 0);
        int has_version = (find_string_field(obj_start, obj_end, "version",
                                              entry.version, sizeof(entry.version)) == 0);
        int has_url = (find_string_field(obj_start, obj_end, "download_url",
                                          entry.download_url, sizeof(entry.download_url)) == 0);

        if (has_name && has_version && has_url) {
            if (find_string_field(obj_start, obj_end, "category",
                                   entry.category, sizeof(entry.category)) != 0) {
                strncpy(entry.category, "generic", sizeof(entry.category) - 1);
            }
            if (find_string_field(obj_start, obj_end, "display_name",
                                   entry.display_name, sizeof(entry.display_name)) != 0) {
                strncpy(entry.display_name, entry.name, sizeof(entry.display_name) - 1);
            }
            find_number_field(obj_start, obj_end, "size_bytes", &entry.size_bytes);
            out[count++] = entry;
        }
        /* else: skip this entry silently, matches CI's schema-check-is-separate
         * philosophy — the device isn't the schema enforcer, just skip and move on */

        p = obj_end;
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
        }
    }

    return count;
}
