/**
 * @file manifest_parser.c
 * @brief WASM manifest parser implementation
 *
 * Parses akira.manifest custom section from WASM binaries and
 * extracts capability masks and memory quotas.
 *
 * WASM binary format reference:
 * - Magic: 0x00 0x61 0x73 0x6d (\0asm)
 * - Version: 0x01 0x00 0x00 0x00
 * - Sections: id (1 byte) + size (LEB128) + content
 * - Custom section id = 0, content starts with name (string)
 */

#include "manifest_parser.h"
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include "runtime/security.h"

LOG_MODULE_REGISTER(manifest_parser, CONFIG_AKIRA_LOG_LEVEL);

/* WASM section IDs */
#define WASM_SECTION_CUSTOM     0
#define WASM_SECTION_TYPE       1
#define WASM_SECTION_IMPORT     2
#define WASM_SECTION_FUNCTION   3
#define WASM_SECTION_TABLE      4
#define WASM_SECTION_MEMORY     5
#define WASM_SECTION_GLOBAL     6
#define WASM_SECTION_EXPORT     7
#define WASM_SECTION_START      8
#define WASM_SECTION_ELEMENT    9
#define WASM_SECTION_CODE       10
#define WASM_SECTION_DATA       11

/* Custom section name we're looking for */
#define AKIRA_MANIFEST_SECTION  ".akira.manifest"

/**
 * @brief Read LEB128 unsigned integer from WASM binary
 */
static size_t read_leb128_u32(const uint8_t *data, size_t max_len, uint32_t *result)
{
    uint32_t value = 0;
    size_t shift = 0;
    size_t i = 0;

    while (i < max_len && i < 5) {  /* Max 5 bytes for u32 LEB128 */
        uint8_t byte = data[i++];
        value |= (uint32_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *result = value;
            return i;
        }
        shift += 7;
    }

    return 0;  /* Error: incomplete LEB128 */
}

/**
 * @brief Skip whitespace in JSON string
 */
static const char *json_skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

/**
 * @brief Parse a JSON string value (allocates memory)
 */
static const char *json_parse_string(const char *p, const char *end, char *out, size_t out_size)
{
    if (p >= end || *p != '"') return NULL;
    p++;

    size_t i = 0;
    while (p < end && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                case '"': out[i++] = '"'; break;
                case '\\': out[i++] = '\\'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';

    if (p >= end || *p != '"') return NULL;
    return p + 1;
}

/**
 * @brief Parse a JSON integer value
 */
static const char *json_parse_int(const char *p, const char *end, uint32_t *out)
{
    p = json_skip_ws(p, end);
    
    uint32_t value = 0;
    bool found = false;

    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
        found = true;
    }

    if (!found) return NULL;
    *out = value;
    return p;
}

void manifest_init_defaults(akira_manifest_t *manifest)
{
    if (!manifest) return;

    memset(manifest, 0, sizeof(*manifest));
    manifest->cap_mask = 0;
    manifest->memory_quota = 0;  /* 0 = use default */
    manifest->name[0] = '\0';
    manifest->version[0] = '\0';
    manifest->valid = false;
}

int manifest_parse_json(const char *json, size_t json_len, akira_manifest_t *manifest)
{
    if (!json || json_len == 0 || !manifest) {
        return -EINVAL;
    }

    manifest_init_defaults(manifest);

    const char *p = json;
    const char *end = json + json_len;
    char key[32];
    char str_val[64];

    p = json_skip_ws(p, end);
    if (p >= end || *p != '{') {
        LOG_ERR("Invalid JSON: expected '{'");
        return -EINVAL;
    }
    p++;

    while (p < end) {
        p = json_skip_ws(p, end);
        if (p >= end) break;
        
        if (*p == '}') {
            /* End of object */
            manifest->valid = true;
            return 0;
        }

        if (*p == ',') {
            p++;
            continue;
        }

        /* Parse key */
        p = json_parse_string(p, end, key, sizeof(key));
        if (!p) {
            LOG_ERR("Invalid JSON: expected key string");
            return -EINVAL;
        }

        p = json_skip_ws(p, end);
        if (p >= end || *p != ':') {
            LOG_ERR("Invalid JSON: expected ':'");
            return -EINVAL;
        }
        p++;
        p = json_skip_ws(p, end);

        /* Parse value based on key */
        if (strcmp(key, "name") == 0) {
            p = json_parse_string(p, end, manifest->name, sizeof(manifest->name));
            if (!p) return -EINVAL;
        }
        else if (strcmp(key, "version") == 0) {
            p = json_parse_string(p, end, manifest->version, sizeof(manifest->version));
            if (!p) return -EINVAL;
        }
        else if (strcmp(key, "memory_quota") == 0) {
            p = json_parse_int(p, end, &manifest->memory_quota);
            if (!p) return -EINVAL;
        }
        else if (strcmp(key, "capabilities") == 0) {
            /* Parse array of capability strings */
            p = json_skip_ws(p, end);
            if (p >= end || *p != '[') {
                LOG_ERR("Invalid JSON: capabilities must be array");
                return -EINVAL;
            }
            p++;

            while (p < end) {
                p = json_skip_ws(p, end);
                if (p >= end) break;

                if (*p == ']') {
                    p++;
                    break;
                }

                if (*p == ',') {
                    p++;
                    continue;
                }

                p = json_parse_string(p, end, str_val, sizeof(str_val));
                if (!p) {
                    LOG_ERR("Invalid capability string");
                    return -EINVAL;
                }

                uint32_t cap = akira_capability_str_to_mask(str_val);
                manifest->cap_mask |= cap;
                LOG_DBG("Parsed capability: %s -> 0x%08x", str_val, cap);
            }
        }
        else {
            /* Skip unknown keys - find end of value */
            int depth = 0;
            bool in_string = false;
            while (p < end) {
                if (!in_string) {
                    if (*p == '"') in_string = true;
                    else if (*p == '{' || *p == '[') depth++;
                    else if (*p == '}' || *p == ']') {
                        if (depth == 0) break;
                        depth--;
                    }
                    else if (*p == ',' && depth == 0) break;
                } else {
                    if (*p == '\\' && p + 1 < end) p++;
                    else if (*p == '"') in_string = false;
                }
                p++;
            }
        }
    }

    manifest->valid = true;
    LOG_INF("Parsed manifest: name=%s, cap_mask=0x%08x, memory_quota=%u",
            manifest->name, manifest->cap_mask, manifest->memory_quota);

    return 0;
}

/**
 * @brief Parse manifest from a WASM binary custom section.
 *
 * WASM sections: 1-byte section_id + LEB128 size + body.
 * Custom sections (id 0): LEB128 name_len + name + content.
 */
static int manifest_parse_wasm_binary(const uint8_t *data, size_t size,
                                      akira_manifest_t *manifest)
{
    size_t pos = 8; /* skip magic + version */

    while (pos < size) {
        uint8_t section_id = data[pos++];
        if (pos >= size) break;

        uint32_t section_size;
        size_t leb_len = read_leb128_u32(data + pos, size - pos, &section_size);
        if (leb_len == 0) {
            LOG_ERR("Invalid section size LEB128");
            return -EINVAL;
        }
        pos += leb_len;

        if (pos + section_size > size) {
            LOG_ERR("Section extends past end of file");
            return -EINVAL;
        }

        if (section_id == WASM_SECTION_CUSTOM) {
            const uint8_t *sd = data + pos;
            size_t sr = section_size;

            uint32_t name_len;
            leb_len = read_leb128_u32(sd, sr, &name_len);
            if (leb_len == 0 || leb_len + name_len > sr) {
                pos += section_size;
                continue;
            }
            sd += leb_len;
            sr -= leb_len;

            if (name_len == strlen(AKIRA_MANIFEST_SECTION) &&
                memcmp(sd, AKIRA_MANIFEST_SECTION, name_len) == 0) {
                sd += name_len;
                sr -= name_len;
                LOG_INF("Found akira.manifest section (%zu bytes)", sr);
                return manifest_parse_json((const char *)sd, sr, manifest);
            }
        }
        pos += section_size;
    }

    LOG_INF("akira.manifest section not found in WASM");
    return -ENOENT;
}

/* AOT section types (from WAMR aot_runtime.h) */
#define AOT_SECTION_TYPE_CUSTOM  100

/**
 * @brief Parse manifest from an AOT binary custom section.
 *
 * AOT sections: uint32 type + uint32 size + body (4-byte aligned).
 * Custom sections (type 100) with sub_type 0 (RAW):
 *   uint32 sub_type + uint16 name_len + name (NUL-terminated) + content.
 */
static int manifest_parse_aot_binary(const uint8_t *data, size_t size,
                                     akira_manifest_t *manifest)
{
    size_t pos = 8; /* skip magic + version */

    while (pos + 8 <= size) {
        /* AOT loader aligns to 4 bytes before each uint32 read */
        pos = (pos + 3) & ~(size_t)3;
        if (pos + 8 > size) break;

        uint32_t sec_type, sec_size;
        memcpy(&sec_type, data + pos, 4);
        memcpy(&sec_size, data + pos + 4, 4);
        pos += 8; /* body starts here */

        if (pos + sec_size > size) break;

        if (sec_type == AOT_SECTION_TYPE_CUSTOM && sec_size > 8) {
            const uint8_t *body = data + pos;

            /* sub_section_type: uint32 at offset 0 */
            uint32_t sub_type;
            memcpy(&sub_type, body, 4);

            if (sub_type == 0) { /* AOT_CUSTOM_SECTION_RAW */
                /* name_len: uint16 at offset 4, then name bytes */
                uint16_t name_len;
                memcpy(&name_len, body + 4, 2);

                if (6 + name_len <= sec_size) {
                    const char *name = (const char *)(body + 6);
                    size_t manifest_name_len = strlen(AKIRA_MANIFEST_SECTION);

                    /* name may be NUL-terminated; compare without trailing NUL */
                    if (name_len >= manifest_name_len &&
                        memcmp(name, AKIRA_MANIFEST_SECTION,
                               manifest_name_len) == 0) {
                        const uint8_t *json = body + 6 + name_len;
                        size_t json_len = sec_size - 6 - name_len;
                        LOG_INF("Found akira.manifest in AOT (%zu bytes)",
                                json_len);
                        return manifest_parse_json((const char *)json,
                                                   json_len, manifest);
                    }
                }
            }
        }
        pos += sec_size;
    }

    LOG_INF("akira.manifest section not found in AOT");
    return -ENOENT;
}

int manifest_parse_wasm_section(const uint8_t *wasm_data, size_t wasm_size,
                                akira_manifest_t *manifest)
{
    if (!wasm_data || wasm_size < 8 || !manifest) {
        return -EINVAL;
    }

    manifest_init_defaults(manifest);

    if (memcmp(wasm_data, "\0aot", 4) == 0) {
        return manifest_parse_aot_binary(wasm_data, wasm_size, manifest);
    }

    if (memcmp(wasm_data, "\0asm", 4) == 0) {
        return manifest_parse_wasm_binary(wasm_data, wasm_size, manifest);
    }

    LOG_ERR("Invalid WASM/AOT magic");
    return -EINVAL;
}

int manifest_parse_with_fallback(const uint8_t *wasm_data, size_t wasm_size,
                                 const char *fallback_json, size_t fallback_len,
                                 akira_manifest_t *manifest)
{
    if (!manifest) {
        return -EINVAL;
    }

    manifest_init_defaults(manifest);

    /* Try WASM custom section first */
    if (wasm_data && wasm_size > 0) {
        int ret = manifest_parse_wasm_section(wasm_data, wasm_size, manifest);
        if (ret == 0) {
            LOG_INF("Manifest loaded from WASM custom section");
            return 0;
        }
        /* Section not found or parse error - try fallback */
        LOG_DBG("WASM section parse returned %d, trying fallback", ret);
    }

    /* Try fallback JSON */
    if (fallback_json && fallback_len > 0) {
        int ret = manifest_parse_json(fallback_json, fallback_len, manifest);
        if (ret == 0) {
            LOG_INF("Manifest loaded from fallback JSON");
            return 0;
        }
        return ret;
    }

    /* No manifest available */
    LOG_DBG("No manifest found (WASM section or fallback)");
    return -ENOENT;
}
