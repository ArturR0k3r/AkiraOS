/*
 * test_security.c
 * ztest suite: security
 *
 * Tests for:
 *   - src/runtime/security.c   — capability string mapping, check_native
 *   - src/runtime/security/app_signing_v2.c — hash, signature verify/reject
 *   - src/runtime/security/sandbox.c        — ctx init, syscall deny, WDT kill
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <errno.h>

#include "security.h"
#include "security/app_signing.h"
#include "security/sandbox.h"
#include "security/trust_levels.h"

/* ── capability string → mask ───────────────────────────────────────────── */

ZTEST(security, test_cap_str_known_display)
{
    uint32_t mask = akira_capability_str_to_mask("display.write");

    zassert_equal(mask, AKIRA_CAP_DISPLAY_WRITE,
                  "display.write mask wrong: 0x%08x", mask);
}

ZTEST(security, test_cap_str_known_timer)
{
    uint32_t mask = akira_capability_str_to_mask("timer");

    zassert_equal(mask, AKIRA_CAP_TIMER,
                  "timer mask wrong: 0x%08x", mask);
}

ZTEST(security, test_cap_str_unknown_returns_zero)
{
    zassert_equal(akira_capability_str_to_mask("unknown.cap"), 0U,
                  "unknown cap should map to 0");
    zassert_equal(akira_capability_str_to_mask(""), 0U,
                  "empty string should map to 0");
    zassert_equal(akira_capability_str_to_mask(NULL), 0U,
                  "NULL should map to 0");
}

ZTEST(security, test_cap_wildcard_storage)
{
    uint32_t mask = akira_capability_str_to_mask("storage.*");
    uint32_t expected = AKIRA_CAP_STORAGE_READ | AKIRA_CAP_STORAGE_WRITE;

    zassert_equal(mask, expected,
                  "storage.* mask wrong: got 0x%08x want 0x%08x",
                  mask, expected);
}

ZTEST(security, test_cap_wildcard_gpio)
{
    uint32_t mask = akira_capability_str_to_mask("gpio.*");
    uint32_t expected = AKIRA_CAP_GPIO_READ | AKIRA_CAP_GPIO_WRITE;

    zassert_equal(mask, expected,
                  "gpio.* mask wrong: 0x%08x", mask);
}

ZTEST(security, test_cap_wildcard_input)
{
    uint32_t mask = akira_capability_str_to_mask("input.*");
    uint32_t expected = AKIRA_CAP_INPUT_READ | AKIRA_CAP_INPUT_WRITE;

    zassert_equal(mask, expected,
                  "input.* mask wrong: 0x%08x", mask);
}

/* High-bit capabilities (bits 31-35). Regression cover for the ota.trigger
 * gap (its manifest string used to map to 0, so OTA could never be granted)
 * and for 64-bit handling of caps above bit 31. */
ZTEST(security, test_cap_str_ota_trigger)
{
	uint64_t mask = akira_capability_str_to_mask("ota.trigger");

	zassert_equal(mask, AKIRA_CAP_OTA_TRIGGER,
		      "ota.trigger must map to its bit-31 mask, got 0x%016llx",
		      (unsigned long long)mask);
	zassert_not_equal(mask, 0ULL, "ota.trigger must not map to 0");
}

ZTEST(security, test_cap_str_high_bits)
{
	uint64_t aiinfer = akira_capability_str_to_mask("ai.infer");
	uint64_t matter  = akira_capability_str_to_mask("matter");
	uint64_t inject  = akira_capability_str_to_mask("wifi.inject");
	uint64_t mqtt    = akira_capability_str_to_mask("mqtt");

	zassert_equal(aiinfer, AKIRA_CAP_AIINFER, "ai.infer mask wrong");
	zassert_equal(matter,  AKIRA_CAP_MATTER,  "matter mask wrong");
	zassert_equal(inject,  AKIRA_CAP_WIFI_INJECT, "wifi.inject mask wrong");
	zassert_equal(mqtt,    AKIRA_CAP_MQTT,
		      "mqtt mask wrong (bit 35 requires a 64-bit mask)");

	zassert_true(aiinfer && matter && inject && mqtt,
		     "high-bit caps must all be non-zero");
	zassert_not_equal(aiinfer, matter, "caps must be distinct");
	zassert_not_equal(inject, mqtt, "caps must be distinct");
}

ZTEST(security, test_cap_native_check_always_permits)
{
    /*
     * With CONFIG_AKIRA_WASM_RUNTIME=n the native check returns true for
     * all capabilities (non-WASM callers are trusted by default).
     */
    zassert_true(akira_security_check_native(AKIRA_CAP_DISPLAY_WRITE),
                 "native check should permit display.write");
    zassert_true(akira_security_check_native(AKIRA_CAP_STORAGE_READ),
                 "native check should permit storage.read");
    zassert_true(akira_security_check_native(0xFFFFFFFFU),
                 "native check should permit all caps");
}

/* ── SHA-256 hashing ────────────────────────────────────────────────────── */

ZTEST(security, test_hash_null_inputs)
{
    uint8_t hash[32];
    uint8_t data[16] = {0};

    zassert_not_equal(app_compute_hash(NULL, 16, hash), 0,
                      "NULL data should fail");
    zassert_not_equal(app_compute_hash(data, 0, hash), 0,
                      "zero length should fail");
    zassert_not_equal(app_compute_hash(data, 16, NULL), 0,
                      "NULL hash output should fail");
}

ZTEST(security, test_hash_known_vector)
{
    /*
     * SHA-256("abc") — NIST FIPS 180-4 / CAVP known-answer test vector:
     * ba7816bf 8f01cfea 414140de 5dae2223
     * b00361a3 96177a9c b410ff61 f20015ad
     */
    static const uint8_t input[] = {'a', 'b', 'c'};
    static const uint8_t expected[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};

    uint8_t hash[32];
    int rc = app_compute_hash(input, sizeof(input), hash);

    /* If mbedTLS is not available the function returns -ENOTSUP — skip. */
    if (rc == -ENOTSUP)
    {
        ztest_test_skip();
    }
    zassert_equal(rc, 0, "hash failed: %d", rc);
    zassert_mem_equal(hash, expected, 32, "SHA-256('abc') mismatch");
}

ZTEST(security, test_hash_deterministic)
{
    static const uint8_t data[] = "AkiraOS test payload";
    uint8_t h1[32], h2[32];

    int rc = app_compute_hash(data, sizeof(data), h1);

    if (rc == -ENOTSUP)
    {
        ztest_test_skip();
    }
    zassert_equal(rc, 0);

    rc = app_compute_hash(data, sizeof(data), h2);
    zassert_equal(rc, 0);

    zassert_mem_equal(h1, h2, 32,
                      "Same input must produce same hash");
}

ZTEST(security, test_hash_differs_on_tamper)
{
    uint8_t data1[] = "AkiraOS original";
    uint8_t data2[] = "AkiraOS tampered";
    uint8_t h1[32], h2[32];

    int rc = app_compute_hash(data1, sizeof(data1), h1);

    if (rc == -ENOTSUP)
    {
        ztest_test_skip();
    }
    zassert_equal(rc, 0);

    rc = app_compute_hash(data2, sizeof(data2), h2);
    zassert_equal(rc, 0);

    zassert_true(memcmp(h1, h2, 32) != 0,
                 "Different inputs must produce different hashes");
}

/* ── app_verify_signature ────────────────────────────────────────────────── */

static void *signing_setup(void)
{
    app_signing_init();
    return NULL;
}

ZTEST(security_signing, test_signing_none_permitted)
{
    /*
     * SIGN_ALG_NONE without CONFIG_AKIRA_APP_SIGNING → allowed (returns 0).
     */
    static const uint8_t binary[] = {0x00, 0x61, 0x73, 0x6D};
    akira_app_signature_t sig = {.algorithm = SIGN_ALG_NONE};

    int rc = app_verify_signature(binary, sizeof(binary), &sig);

    zassert_equal(rc, 0,
                  "unsigned app should be allowed when signing not enforced");
}

ZTEST(security_signing, test_signing_untrusted_cert_rejected)
{
    /*
     * RSA signature with a cert_hash that has NOT been added to trusted
     * roots → must be rejected with -EACCES.
     */
    static const uint8_t binary[] = {0x00, 0x61, 0x73, 0x6D, 0x01};
    akira_app_signature_t sig = {
        .algorithm = SIGN_ALG_RSA2048_SHA256,
        .signature_len = 0,
        /* cert_hash all-zeros: not in trusted roots */
    };
    memset(sig.cert_hash, 0x00, sizeof(sig.cert_hash));

    int rc = app_verify_signature(binary, sizeof(binary), &sig);

    if (rc == -ENOTSUP)
    {
        ztest_test_skip(); /* crypto not available */
    }
    zassert_equal(rc, -EACCES,
                  "untrusted cert should be rejected (-EACCES), got %d", rc);
}

ZTEST(security_signing, test_signing_ed25519_untrusted_cert_rejected)
{
    static const uint8_t binary[] = {0x00, 0x61, 0x73, 0x6D, 0x01};
    akira_app_signature_t sig = {
        .algorithm = SIGN_ALG_ED25519,
        .signature_len = 64,
    };
    memset(sig.cert_hash, 0xAB, sizeof(sig.cert_hash)); /* not trusted */

    int rc = app_verify_signature(binary, sizeof(binary), &sig);

    if (rc == -ENOTSUP)
    {
        ztest_test_skip();
    }
    zassert_equal(rc, -EACCES,
                  "ed25519 with untrusted cert should fail, got %d", rc);
}

ZTEST(security_signing, test_signing_null_inputs)
{
    static const uint8_t binary[] = {0x00, 0x61, 0x73, 0x6D};
    akira_app_signature_t sig = {.algorithm = SIGN_ALG_NONE};

    zassert_not_equal(app_verify_signature(NULL, 4, &sig), 0,
                      "NULL binary should fail");
    zassert_not_equal(app_verify_signature(binary, 0, &sig), 0,
                      "zero size should fail");
    zassert_not_equal(app_verify_signature(binary, 4, NULL), 0,
                      "NULL sig should fail");
}

/* ── sandbox ─────────────────────────────────────────────────────────────── */

ZTEST(security_sandbox, test_sandbox_init_user_trust)
{
    sandbox_ctx_t ctx;

    sandbox_ctx_init(&ctx, TRUST_LEVEL_USER, 0);

    zassert_true(ctx.initialized, "context should be initialized");
    zassert_equal(ctx.trust_level, TRUST_LEVEL_USER, "trust level wrong");
    zassert_false(ctx.exec_active, "exec_active should start false");
    zassert_equal(ctx.watchdog_kills, 0U, "watchdog_kills should be 0");
}

ZTEST(security_sandbox, test_sandbox_allows_display_with_cap)
{
    sandbox_ctx_t ctx;

    /* User trust + display cap → SYSCALL_CAT_DISPLAY must be allowed */
    sandbox_ctx_init(&ctx, TRUST_LEVEL_USER, AKIRA_CAP_DISPLAY_WRITE);

    bool allowed = sandbox_check_syscall(&ctx, SYSCALL_CAT_DISPLAY, "test");

    zassert_true(allowed, "display syscall should be allowed with cap");
}

ZTEST(security_sandbox, test_sandbox_denies_rf_without_cap)
{
    sandbox_ctx_t ctx;

    /* User trust, no RF cap */
    sandbox_ctx_init(&ctx, TRUST_LEVEL_USER, 0);

    bool allowed = sandbox_check_syscall(&ctx, SYSCALL_CAT_RF, "test");

    zassert_false(allowed, "RF syscall should be denied without cap");
    zassert_equal(ctx.denied_syscalls, 1U, "denied_syscalls should be 1");
}

ZTEST(security_sandbox, test_sandbox_watchdog_kill)
{
    sandbox_ctx_t ctx;

    sandbox_ctx_init(&ctx, TRUST_LEVEL_USER, 0);

    /* Simulate active execution */
    sandbox_exec_begin(&ctx);
    zassert_true(ctx.exec_active, "exec should be active");

    /* Kill via watchdog */
    sandbox_watchdog_kill(&ctx, "test_app");

    zassert_false(ctx.exec_active,
                  "exec_active should be false after watchdog kill");
    zassert_equal(ctx.watchdog_kills, 1U,
                  "watchdog_kills should be 1");
}

ZTEST(security_sandbox, test_sandbox_watchdog_kill_increments_count)
{
    sandbox_ctx_t ctx;

    sandbox_ctx_init(&ctx, TRUST_LEVEL_USER, 0);

    sandbox_watchdog_kill(&ctx, "app");
    sandbox_watchdog_kill(&ctx, "app");

    zassert_equal(ctx.watchdog_kills, 2U,
                  "watchdog_kills should be 2 after two kills");
}

ZTEST(security_sandbox, test_sandbox_exec_timeout_default)
{
    sandbox_ctx_t ctx;

    sandbox_ctx_init(&ctx, TRUST_LEVEL_USER, 0);

    zassert_equal(ctx.exec_timeout_ms,
                  (uint32_t)CONFIG_AKIRA_SANDBOX_EXEC_TIMEOUT_MS,
                  "default timeout mismatch");
}

ZTEST(security_sandbox, test_sandbox_kernel_trust_allows_all)
{
    sandbox_ctx_t ctx;

    sandbox_ctx_init(&ctx, TRUST_LEVEL_KERNEL, 0);

    /* Kernel trust should allow every syscall category */
    zassert_true(sandbox_check_syscall(&ctx, SYSCALL_CAT_RF, "k"),
                 "kernel should allow RF");
    zassert_true(sandbox_check_syscall(&ctx, SYSCALL_CAT_SYSTEM, "k"),
                 "kernel should allow SYSTEM");
}

/* ── suite registration ─────────────────────────────────────────────────── */

ZTEST_SUITE(security, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(security_signing, NULL, signing_setup, NULL, NULL, NULL);
ZTEST_SUITE(security_sandbox, NULL, NULL, NULL, NULL, NULL);
