/**
 * @file manifest_parser.h
 * @brief WASM manifest parser for AkiraOS
 *
 * Parses capability masks and memory quotas from:
 * 1. WASM custom section named "akira-manifest"
 * 2. Fallback to external .json manifest file
 *
 * The akira-manifest custom section format:
 * - JSON encoded in UTF-8
 * - Contains: capabilities[], memory_quota, name, version
 */

#ifndef MANIFEST_PARSER_H
#define MANIFEST_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parsed manifest data structure
 */
typedef struct {
    uint32_t cap_mask;          /**< Capability bitmask */
    uint32_t memory_quota;      /**< Memory quota in bytes (0 = default) */
    char name[32];              /**< Application name */
    char version[16];           /**< Version string (e.g., "1.0.0") */
    bool valid;                 /**< True if manifest was successfully parsed */
} akira_manifest_t;

/**
 * @brief Parse manifest from WASM binary custom section
 *
 * Searches for custom section named "akira-manifest" in the WASM binary
 * and parses the JSON content to extract capabilities and quotas.
 *
 * @param wasm_data Pointer to WASM binary data
 * @param wasm_size Size of WASM binary
 * @param manifest Output manifest structure
 * @return 0 on success, negative errno on failure
 *         -ENOENT: custom section not found
 *         -EINVAL: invalid WASM format or malformed JSON
 */
int manifest_parse_wasm_section(const uint8_t *wasm_data, size_t wasm_size,
                                akira_manifest_t *manifest);

/**
 * @brief Parse manifest from JSON string
 *
 * Parses a JSON manifest string directly. Used for external .json files
 * or embedded JSON in WASM custom sections.
 *
 * @param json JSON string
 * @param json_len Length of JSON string
 * @param manifest Output manifest structure
 * @return 0 on success, negative errno on failure
 */
int manifest_parse_json(const char *json, size_t json_len,
                        akira_manifest_t *manifest);

/**
 * @brief Parse manifest with fallback strategy
 *
 * First attempts to parse from WASM custom section. If not found,
 * falls back to provided JSON string (typically from external .json file).
 *
 * @param wasm_data Pointer to WASM binary data (can be NULL to skip)
 * @param wasm_size Size of WASM binary
 * @param fallback_json Fallback JSON string (can be NULL)
 * @param fallback_len Length of fallback JSON
 * @param manifest Output manifest structure
 * @return 0 on success (from either source), negative errno if both fail
 */
int manifest_parse_with_fallback(const uint8_t *wasm_data, size_t wasm_size,
                                 const char *fallback_json, size_t fallback_len,
                                 akira_manifest_t *manifest);

/**
 * @brief Initialize manifest with defaults
 *
 * @param manifest Manifest structure to initialize
 */
void manifest_init_defaults(akira_manifest_t *manifest);

/**
 * @brief Convert capability string to mask bit
 *
 * @param capability Capability string (e.g., "display.write")
 * @return Capability mask bit, or 0 if unknown
 */
uint32_t akira_capability_str_to_mask(const char *cap);

/**
 * @brief Get capability name from mask bit
 *
 * @param mask Single capability bit
 * @return Capability string, or NULL if unknown
 */
const char *manifest_mask_to_capability(uint32_t mask);

#ifdef __cplusplus
}
#endif

#endif /* MANIFEST_PARSER_H */
