/* Minimal JSON capability parser for AkiraOS
 * Lightweight, dependency-free parser that extracts the
 * "capabilities" array of strings and computes a capability mask.
 * This intentionally supports only the subset we need (array of strings)
 * and is robust against whitespace and simple escapes.
 */

#ifndef SIMPLE_JSON_H
#define SIMPLE_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a JSON document and extract capability bitmask.
 * Returns a uint32_t mask where bits are as per akira cap mapping.
 * The function is forgiving for whitespace and simple JSON strings.
 */
uint32_t parse_capabilities_mask(const char *json, size_t json_len);

/* Lightweight helpers for simple manifest parsing */
int simple_json_get_string(const char *json, size_t json_len, const char *key, char *out, size_t out_len);
int simple_json_get_int(const char *json, size_t json_len, const char *key, int *out);

#ifdef __cplusplus
}
#endif

#endif /* SIMPLE_JSON_H */
