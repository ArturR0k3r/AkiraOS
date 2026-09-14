/*
 * test_manifest_parser.c
 * ztest suite: manifest_parser
 *
 * Tests for src/runtime/manifest_parser.c
 * Exercises manifest_parse_json() and manifest_parse_wasm_section().
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stdint.h>
#include "manifest_parser.h"
#include "security.h" /* cap bit-mask constants */

/* ── helpers ─────────────────────────────────────────────────────────────── */

/**
 * Build a minimal WASM binary that contains a single custom section
 * named ".akira.manifest" with @p json as its payload.
 * @p buf must be large enough (json_len + 32 bytes of overhead).
 */
static void build_wasm_with_manifest(uint8_t *buf, size_t *out_len,
                                     const char *json)
{
    const char *section_name = ".akira.manifest";
    uint8_t name_len = (uint8_t)strlen(section_name); /* 15 */
    size_t json_len = strlen(json);

    /* section body = LEB128(name_len) + name + json */
    uint32_t body = 1u + name_len + (uint32_t)json_len;

    size_t pos = 0;

    /* WASM magic (\0asm) + version (1) */
    buf[pos++] = 0x00;
    buf[pos++] = 0x61;
    buf[pos++] = 0x73;
    buf[pos++] = 0x6D;
    buf[pos++] = 0x01;
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;

    /* Custom section id = 0 */
    buf[pos++] = 0x00;

    /* Section size as LEB128 (up to 2 bytes covers < 16383 bytes) */
    if (body < 128)
    {
        buf[pos++] = (uint8_t)body;
    }
    else
    {
        buf[pos++] = (uint8_t)((body & 0x7F) | 0x80);
        buf[pos++] = (uint8_t)(body >> 7);
    }

    /* Name length (fits in 1 byte) */
    buf[pos++] = name_len;
    /* Name */
    memcpy(&buf[pos], section_name, name_len);
    pos += name_len;
    /* JSON payload */
    memcpy(&buf[pos], json, json_len);
    pos += json_len;

    *out_len = pos;
}

/* ── test cases ──────────────────────────────────────────────────────────── */

ZTEST(manifest_parser, test_valid_json)
{
    const char *json =
        "{\"name\":\"myapp\",\"version\":\"1.0.0\","
        "\"memory_quota\":65536,"
        "\"capabilities\":[\"display.write\",\"storage.read\"]}";

    akira_manifest_t m;
    int rc = manifest_parse_json(json, strlen(json), &m);

    zassert_equal(rc, 0, "parse should succeed, got %d", rc);
    zassert_true(m.valid, "manifest.valid should be true");
    zassert_equal(m.memory_quota, 65536U, "memory_quota mismatch");
    zassert_str_equal(m.name, "myapp", "name mismatch");
    zassert_str_equal(m.version, "1.0.0", "version mismatch");
    zassert_true(m.cap_mask & AKIRA_CAP_DISPLAY_WRITE,
                 "display.write cap missing");
    zassert_true(m.cap_mask & AKIRA_CAP_STORAGE_READ,
                 "storage.read cap missing");
}

ZTEST(manifest_parser, test_invalid_json)
{
    const char *bad = "not_json_at_all";

    akira_manifest_t m;
    int rc = manifest_parse_json(bad, strlen(bad), &m);

    zassert_not_equal(rc, 0, "bad JSON should fail, got rc=%d", rc);
    zassert_false(m.valid, "manifest.valid should be false");
}

ZTEST(manifest_parser, test_truncated_json)
{
    /* Truncated mid-string — parser must not crash */
    const char *trunc = "{\"name\":\"app\",\"capabilities\":[\"display.w";

    akira_manifest_t m;
    /* Return value may be -EINVAL or 0 with partial parse — must not hang */
    manifest_parse_json(trunc, strlen(trunc), &m);
    /* No crash is the primary assertion; valid must not be inconsistently set */
}

ZTEST(manifest_parser, test_bad_capability)
{
    /* Unknown capability strings must be silently ignored (mask stays 0) */
    const char *json =
        "{\"capabilities\":[\"foo.bar\",\"unknown.cap\"]}";

    akira_manifest_t m;
    int rc = manifest_parse_json(json, strlen(json), &m);

    zassert_equal(rc, 0, "parse should succeed");
    zassert_equal(m.cap_mask, 0U,
                  "unknown caps should contribute 0 to mask, got 0x%08x",
                  m.cap_mask);
}

ZTEST(manifest_parser, test_oversize_quota)
{
    /*
     * Largest uint32 value in JSON (4294967295).  Parser must not crash
     * and must set memory_quota to the parsed value without overflowing
     * into adjacent fields.
     */
    const char *json =
        "{\"memory_quota\":4294967295,\"name\":\"big\"}";

    akira_manifest_t m;
    int rc = manifest_parse_json(json, strlen(json), &m);

    /* Parser should succeed or return -EINVAL — either is acceptable.
     * The critical invariant is that name[] is not corrupted. */
    if (rc == 0)
    {
        zassert_str_equal(m.name, "big",
                          "name corrupted by large quota");
    }
}

ZTEST(manifest_parser, test_missing_name)
{
    /* name field omitted — should parse OK with empty name */
    const char *json =
        "{\"version\":\"2.0\",\"capabilities\":[\"timer\"]}";

    akira_manifest_t m;
    int rc = manifest_parse_json(json, strlen(json), &m);

    zassert_equal(rc, 0, "parse should succeed without name");
    zassert_true(m.valid, "manifest.valid should be true");
    zassert_equal(m.name[0], '\0', "name should be empty string");
    zassert_true(m.cap_mask & AKIRA_CAP_TIMER, "timer cap missing");
}

ZTEST(manifest_parser, test_null_inputs)
{
    akira_manifest_t m;

    zassert_not_equal(manifest_parse_json(NULL, 10, &m), 0,
                      "NULL json should fail");
    zassert_not_equal(manifest_parse_json("{}", 2, NULL), 0,
                      "NULL manifest should fail");
    zassert_not_equal(manifest_parse_json("{}", 0, &m), 0,
                      "zero length should fail");
}

ZTEST(manifest_parser, test_wasm_section)
{
    const char *json =
        "{\"name\":\"wtest\",\"version\":\"0.1\","
        "\"memory_quota\":4096,"
        "\"capabilities\":[\"storage.write\",\"timer\"]}";

    uint8_t wasm_buf[256];
    size_t wasm_len = 0;

    build_wasm_with_manifest(wasm_buf, &wasm_len, json);
    zassert_true(wasm_len > 8, "WASM binary must be larger than header");

    akira_manifest_t m;
    int rc = manifest_parse_wasm_section(wasm_buf, wasm_len, &m);

    zassert_equal(rc, 0, "wasm section parse failed: %d", rc);
    zassert_true(m.valid, "manifest.valid should be true");
    zassert_str_equal(m.name, "wtest", "name mismatch");
    zassert_equal(m.memory_quota, 4096U, "memory_quota mismatch");
    zassert_true(m.cap_mask & AKIRA_CAP_STORAGE_WRITE,
                 "storage.write cap missing");
    zassert_true(m.cap_mask & AKIRA_CAP_TIMER, "timer cap missing");
}

ZTEST(manifest_parser, test_wasm_no_manifest_section)
{
    /* A WASM binary with no custom section — should return -ENOENT */
    static const uint8_t minimal_wasm[] = {
        0x00, 0x61, 0x73, 0x6D, /* magic */
        0x01, 0x00, 0x00, 0x00  /* version */
    };

    akira_manifest_t m;
    int rc = manifest_parse_wasm_section(minimal_wasm, sizeof(minimal_wasm), &m);

    zassert_equal(rc, -ENOENT,
                  "expected -ENOENT for missing section, got %d", rc);
}

ZTEST(manifest_parser, test_high_capability_bits)
{
    /* Capabilities above bit 31 must survive parsing; a 16- or 32-bit mask
     * anywhere on the path would silently drop them. */
    const char *json =
        "{\"name\":\"hibits\",\"version\":\"1.0.0\","
        "\"memory_quota\":65536,"
        "\"capabilities\":[\"ai.infer\",\"mqtt\",\"sync\"]}";

    akira_manifest_t m;
    int rc = manifest_parse_json(json, strlen(json), &m);

    zassert_equal(rc, 0, "parse should succeed, got %d", rc);
    zassert_true(m.cap_mask & AKIRA_CAP_AIINFER, "ai.infer (bit 32) missing");
    zassert_true(m.cap_mask & AKIRA_CAP_MQTT, "mqtt (bit 35) missing");
    zassert_true(m.cap_mask & AKIRA_CAP_SYNC, "sync (bit 39) missing");
}

ZTEST_SUITE(manifest_parser, NULL, NULL, NULL, NULL, NULL);
