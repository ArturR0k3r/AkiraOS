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
#include "akira_abi_check.h"
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

ZTEST(manifest_parser, test_abi_and_min_version_parsed)
{
    const char *json =
        "{\"name\":\"v\",\"abi\":\"1.2\",\"min_akiraos_version\":\"1.6.0\"}";
    akira_manifest_t m;

    zassert_equal(manifest_parse_json(json, strlen(json), &m), 0, "parse failed");
    zassert_true(m.has_abi, "abi flag");
    zassert_equal(m.abi_major, 1, "abi major");
    zassert_equal(m.abi_minor, 2, "abi minor");
    zassert_true(m.has_min_os, "min_os flag");
    zassert_equal(m.min_os[0], 1, "min major");
    zassert_equal(m.min_os[1], 6, "min minor");
}

ZTEST(manifest_parser, test_no_abi_keys)
{
    const char *json = "{\"name\":\"v\"}";
    akira_manifest_t m;

    zassert_equal(manifest_parse_json(json, strlen(json), &m), 0, "parse failed");
    zassert_false(m.has_abi, "no abi key");
    zassert_false(m.has_min_os, "no min_os key");
}

/* ── ABI gate (akira_abi_check) ─────────────────────────────────────────── */

static akira_manifest_t abi_manifest(bool has_abi, int amaj, int amin,
                                     bool has_min, int mj, int mn, int mp)
{
    akira_manifest_t m = { .valid = true, .has_abi = has_abi,
                           .abi_major = amaj, .abi_minor = amin,
                           .has_min_os = has_min };
    m.min_os[0] = mj; m.min_os[1] = mn; m.min_os[2] = mp;
    return m;
}

ZTEST(manifest_parser, test_abi_gate)
{
    const uint16_t fw[3] = {1, 6, 4};

    akira_manifest_t same = abi_manifest(true, 1, 0, false, 0, 0, 0);
    zassert_equal(akira_abi_check(&same, 1, 0, fw), 0, "same ABI must pass");

    akira_manifest_t older_minor = abi_manifest(true, 1, 0, false, 0, 0, 0);
    zassert_equal(akira_abi_check(&older_minor, 1, 3, fw), 0, "older minor must pass");

    akira_manifest_t newer_minor = abi_manifest(true, 1, 5, false, 0, 0, 0);
    zassert_equal(akira_abi_check(&newer_minor, 1, 3, fw), 0, "newer minor warns but passes");

    akira_manifest_t major2 = abi_manifest(true, 2, 0, false, 0, 0, 0);
    zassert_equal(akira_abi_check(&major2, 1, 0, fw), -ENOTSUP, "major mismatch must fail");

    akira_manifest_t none = abi_manifest(false, 0, 0, false, 0, 0, 0);
    zassert_equal(akira_abi_check(&none, 1, 0, fw), 0, "missing ABI is legacy, passes");

    akira_manifest_t need_new = abi_manifest(true, 1, 0, true, 1, 7, 0);
    zassert_equal(akira_abi_check(&need_new, 1, 0, fw), -ENOTSUP,
                  "min_akiraos_version newer than firmware must fail");

    akira_manifest_t need_old = abi_manifest(true, 1, 0, true, 1, 5, 0);
    zassert_equal(akira_abi_check(&need_old, 1, 0, fw), 0,
                  "min_akiraos_version older than firmware passes");

    akira_manifest_t need_patch = abi_manifest(true, 1, 0, true, 1, 6, 9);
    zassert_equal(akira_abi_check(&need_patch, 1, 0, fw), -ENOTSUP,
                  "patch component is compared too");
}

ZTEST_SUITE(manifest_parser, NULL, NULL, NULL, NULL, NULL);
