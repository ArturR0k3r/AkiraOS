/* Minimal JSON capability parser for AkiraOS
 * Lightweight, dependency-free parser that extracts the
 * "capabilities" array of strings and computes a capability mask.
 * This intentionally supports only the subset we need (array of strings)
 * and is robust against whitespace and simple escapes.
 */

/**
 * @file simple_json.h
 * @stability stable
 * @since 1.4
 */
#ifndef SIMPLE_JSON_H
#define SIMPLE_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a JSON document and extract a legacy capability bitmask.
 *
 * @deprecated since 1.6.4; removal no sooner than two minor releases later.
 * Maps only four capability strings, and its bit numbers do not match
 * AKIRA_CAP_* in runtime/security.h. Use manifest_parse_json() from
 * runtime/manifest_parser.h, which returns the 64-bit mask the runtime enforces.
 */
__attribute__((deprecated("use manifest_parse_json() from runtime/manifest_parser.h")))
uint32_t parse_capabilities_mask(const char *json, size_t json_len);

/* Lightweight helpers for simple manifest parsing */
int simple_json_get_string(const char *json, size_t json_len, const char *key, char *out, size_t out_len);
int simple_json_get_int(const char *json, size_t json_len, const char *key, int *out);

#ifdef __cplusplus
}
#endif

#endif /* SIMPLE_JSON_H */
