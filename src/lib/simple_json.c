#include "simple_json.h"
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>

#include <errno.h>

/* Helper: skip whitespace and return new index */
static size_t skip_ws(const char *s, size_t len, size_t i)
{
    while (i < len && isspace((unsigned char)s[i])) i++;
    return i;
}


/* Parse a JSON string starting at i (expects starting '"'),
 * writes an allocated null-terminated C string into *out (caller frees),
 * returns new index after the closing quote or 0 on error.
 */
static size_t parse_json_string(const char *s, size_t len, size_t i, char **out)
{
    if (i >= len || s[i] != '"') return 0;
    i++;
    size_t cap = 32;
    char *buf = malloc(cap);
    if (!buf) return 0;
    size_t bp = 0;

    while (i < len)
    {
        char c = s[i++];
        if (c == '\\') {
            if (i >= len) break;
            char esc = s[i++];
            switch (esc) {
                case '"': buf[bp++] = '"'; break;
                case '/': buf[bp++] = '/'; break;
                case 'b': buf[bp++] = '\b'; break;
                case 'f': buf[bp++] = '\f'; break;
                case 'n': buf[bp++] = '\n'; break;
                case 'r': buf[bp++] = '\r'; break;
                case 't': buf[bp++] = '\t'; break;
                case 'u':
                    /* skip unicode escapes (not needed for capability tokens) */
                    if (i + 4 <= len) i += 4;
                    break;
                default:
                    buf[bp++] = esc; break;
            }
        } else if (c == '"') {
            /* End of string */
            break;
        } else {
            buf[bp++] = c;
        }
        if (bp + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return 0; }
            buf = nb;
        }
    }

    buf[bp] = '\0';
    *out = buf;
    return i;
}

/* Map capability string to mask bits. Keep in sync with akira runtime bits. */
static uint32_t capability_str_to_mask(const char *s)
{
    if (!s) return 0;
    if (strcmp(s, "display.write") == 0) return (1U << 0);
    if (strcmp(s, "input.read") == 0) return (1U << 1);
    if (strcmp(s, "sensor.read") == 0) return (1U << 2);
    if (strcmp(s, "rf.transceive") == 0) return (1U << 3);
    /* Wildcard forms handled by callers if present in tokens */
    if (strcmp(s, "display.*") == 0) return (1U << 0);
    if (strcmp(s, "input.*") == 0) return (1U << 1);
    if (strcmp(s, "sensor.*") == 0) return (1U << 2);
    if (strcmp(s, "rf.*") == 0) return (1U << 3);
    return 0;
}

uint32_t parse_capabilities_mask(const char *json, size_t json_len)
{
    if (!json || json_len == 0) return 0;
    uint32_t mask = 0;
    size_t i = 0;
    i = skip_ws(json, json_len, i);

    /* Quick scan: find "capabilities" token */
    while (i < json_len) {
        if (json[i] == '"') {
            char *key = NULL;
            size_t ni = parse_json_string(json, json_len, i, &key);
            if (!ni) break; /* malformed string */
            if (strcmp(key, "capabilities") == 0) {
                free(key);
                i = skip_ws(json, json_len, ni);
                if (i < json_len && json[i] == ':') {
                    i++;
                    i = skip_ws(json, json_len, i);
                    if (i < json_len && json[i] == '[') {
                        i++;
                        /* parse array */
                        while (i < json_len) {
                            i = skip_ws(json, json_len, i);
                            if (i >= json_len) break;
                            if (json[i] == ']') { i++; break; }
                            if (json[i] == ',') { i++; continue; }
                            if (json[i] == '\"') {
                                char *val = NULL;
                                size_t vi = parse_json_string(json, json_len, i, &val);
                                if (!vi) break; /* bad string */
                                mask |= capability_str_to_mask(val);
                                free(val);
                                i = vi;
                                continue;
                            }
                            /* If value isn't a string - skip until next comma or ] */
                            while (i < json_len && json[i] != ',' && json[i] != ']') i++;
                        }
                        /* done parsing capabilities array */
                        break;
                    }
                }
            } else {
                free(key);
                i = ni;
            }
        } else {
            i++;
        }
    }

    return mask;
}

/* Find a JSON string value for a given key. Writes into out (null-terminated).
 * Returns 0 on success, negative on error/not-found.
 */
int simple_json_get_string(const char *json, size_t json_len, const char *key, char *out, size_t out_len)
{
    if (!json || !key || !out || out_len == 0) return -EINVAL;
    size_t i = 0;
    i = skip_ws(json, json_len, i);

    while (i < json_len) {
        if (json[i] == '"') {
            char *k = NULL;
            size_t ni = parse_json_string(json, json_len, i, &k);
            if (!ni) break;
            int found = 0;
            if (strcmp(k, key) == 0) found = 1;
            free(k);
            if (found) {
                i = skip_ws(json, json_len, ni);
                if (i < json_len && json[i] == ':') i++;
                i = skip_ws(json, json_len, i);
                if (i < json_len && json[i] == '"') {
                    char *val = NULL;
                    size_t vi = parse_json_string(json, json_len, i, &val);
                    if (!vi) return -EIO;
                    strncpy(out, val, out_len - 1);
                    out[out_len - 1] = '\0';
                    free(val);
                    return 0;
                }
                return -ENOENT;
            }
            i = ni;
        } else {
            i++;
        }
    }
    return -ENOENT;
}

/* Find a JSON numeric (integer) value for a given key. Returns 0 on success */
int simple_json_get_int(const char *json, size_t json_len, const char *key, int *out)
{
    if (!json || !key || !out) return -EINVAL;
    char buf[32];
    int r = simple_json_get_string(json, json_len, key, buf, sizeof(buf));
    if (r == 0) {
        *out = atoi(buf);
        return 0;
    }

    /* fallback: look for key and then parse digits following ':' */
    const char *kptr = strstr(json, key);
    if (!kptr) return -ENOENT;
    const char *c = strchr(kptr, ':');
    if (!c) return -ENOENT;
    c++;
    while (*c && isspace((unsigned char)*c)) c++;
    if (!*c) return -ENOENT;
    int sign = 1;
    if (*c == '-') { sign = -1; c++; }
    int val = 0;
    while (*c && isdigit((unsigned char)*c)) { val = val * 10 + (*c - '0'); c++; }
    *out = val * sign;
    return 0;
}
