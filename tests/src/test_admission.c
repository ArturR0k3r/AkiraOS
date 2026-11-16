/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Extension points added in 1.7: app admission (+ the Ed25519 baseline
 * provider), network egress policy and audit sinks.
 *
 * Test vectors come from PyCA cryptography 44 (an independent Ed25519
 * implementation). The key is RFC 8032 section 7.1 test 1; the baseline
 * provider is built with CONFIG_AKIRA_APP_PUBKEY set to its public half
 * (tests/CMakeLists.txt).
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <string.h>

#include <akira_admission.h>
#include <akira_audit.h>
#include <akira_ed25519.h>
#include <akira_net_policy.h>

#include "runtime/security/sandbox.h"

static const uint8_t rfc_pk[32] = {
	0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3,
	0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
	0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
};

/* RFC 8032 test 1: signature of the empty message. */
static const uint8_t rfc_sig_empty[64] = {
	0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc,
	0x80, 0x6e, 0x82, 0x8a, 0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74,
	0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55, 0x5f, 0xb8, 0x82, 0x15,
	0x90, 0xa3, 0x3b, 0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b,
	0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43,
	0x8e, 0x7a, 0x10, 0x0b,
};

/* SHA-256('{"name":"hello","version":"1.0.0"}' || 00 61 73 6d 01 00 00 00):
 * the package digest akira-cli signs. */
static const uint8_t pkg_digest[32] = {
	0x12, 0x53, 0x40, 0x19, 0xaa, 0x29, 0x31, 0x12, 0x5f, 0x7b, 0xc6, 0x11,
	0xc6, 0x2e, 0x9f, 0x00, 0x5e, 0x2a, 0xca, 0xc6, 0x82, 0x74, 0x0c, 0xb0,
	0xe2, 0xec, 0x12, 0x90, 0xf9, 0x3d, 0x2a, 0x22,
};

static const uint8_t pkg_sig[64] = {
	0x1b, 0xd2, 0xc2, 0x3f, 0x2c, 0x52, 0xf0, 0x69, 0x39, 0x54, 0x19, 0x53,
	0xcd, 0x53, 0x15, 0xd6, 0xcc, 0x02, 0x88, 0x84, 0xe2, 0x6d, 0x61, 0xba,
	0x78, 0x7c, 0xb9, 0x02, 0x93, 0x96, 0xc3, 0x8a, 0xcb, 0xb4, 0x91, 0xf1,
	0x39, 0x5e, 0xb4, 0x0d, 0x2c, 0xa7, 0x50, 0x99, 0x97, 0xf9, 0xcc, 0x9a,
	0x04, 0x44, 0x0f, 0x13, 0x81, 0x53, 0xbf, 0x38, 0x05, 0x28, 0xee, 0xaf,
	0x70, 0x37, 0x1b, 0x02,
};

static const uint8_t some_hash[32] = { 1, 2, 3 };

/* ===== Ed25519 ===== */

ZTEST(ed25519, test_rfc8032_vector)
{
	zassert_ok(akira_ed25519_verify(NULL, 0, rfc_sig_empty, rfc_pk));
}

ZTEST(ed25519, test_package_digest_signature)
{
	zassert_ok(akira_ed25519_verify(pkg_digest, 32, pkg_sig, rfc_pk));
}

ZTEST(ed25519, test_rejects_tampering)
{
	uint8_t sig[64];
	uint8_t msg[32];
	uint8_t pk[32];

	memcpy(sig, pkg_sig, 64);
	sig[10] ^= 0x01;
	zassert_equal(akira_ed25519_verify(pkg_digest, 32, sig, rfc_pk), -EBADMSG);

	memcpy(msg, pkg_digest, 32);
	msg[31] ^= 0x80;
	zassert_equal(akira_ed25519_verify(msg, 32, pkg_sig, rfc_pk), -EBADMSG);

	memcpy(pk, rfc_pk, 32);
	pk[0] ^= 0x01;
	zassert_equal(akira_ed25519_verify(pkg_digest, 32, pkg_sig, pk), -EBADMSG);

	/* S >= group order (malleability) must be rejected. */
	memcpy(sig, pkg_sig, 64);
	memset(sig + 32, 0xff, 31);
	sig[63] = 0x0f;
	zassert_equal(akira_ed25519_verify(pkg_digest, 32, sig, rfc_pk), -EBADMSG);

	zassert_equal(akira_ed25519_verify(NULL, 32, pkg_sig, rfc_pk), -EINVAL);
}

ZTEST(ed25519, test_pubkey_hex)
{
	uint8_t pk[32];

	zassert_ok(akira_ed25519_pubkey_from_hex(
		"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", pk));
	zassert_mem_equal(pk, rfc_pk, 32);
	zassert_equal(akira_ed25519_pubkey_from_hex("d75a98", pk), -EINVAL);
	zassert_equal(akira_ed25519_pubkey_from_hex(
		"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a00", pk),
		-EINVAL);
	zassert_equal(akira_ed25519_pubkey_from_hex(
		"z75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", pk),
		-EINVAL);
}

/* ===== Admission ===== */

/* Two test providers bracketing the core baseline (prio 100). Both abstain
 * unless a test arms them. */
static struct {
	int early_verdict;
	bool early_attest;
	uint64_t early_clamp;
	int late_verdict;
	int early_calls;
	int late_calls;
} t;

static int early_admit(const struct akira_admission_ctx *ctx,
		       struct akira_admission_result *res, void *user)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(user);
	t.early_calls++;
	if (t.early_verdict == AKIRA_ADMISSION_DENY) {
		res->reason = "early says no";
	}
	if (t.early_attest) {
		res->attested = true;
		memset(res->signer_id, 0xee, sizeof(res->signer_id));
	}
	res->cap_clamp &= t.early_clamp;
	return t.early_verdict;
}

static int late_admit(const struct akira_admission_ctx *ctx,
		      struct akira_admission_result *res, void *user)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(res);
	ARG_UNUSED(user);
	t.late_calls++;
	return t.late_verdict;
}

/* Defined late-first to check that order follows prio, not link order. */
AKIRA_APP_ADMISSION_DEFINE(test_late_provider, 200, late_admit, NULL);
AKIRA_APP_ADMISSION_DEFINE(test_early_provider, 10, early_admit, NULL);

static void admission_reset(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(&t, 0, sizeof(t));
	t.early_clamp = UINT64_MAX;
}

static struct akira_admission_ctx install_ctx(void)
{
	struct akira_admission_ctx ctx = {
		.phase = AKIRA_ADMISSION_INSTALL,
		.source = AKIRA_APP_SOURCE_HTTP,
		.name = "hello",
		.version = "1.0.0",
		.binary_sha256 = some_hash,
	};
	return ctx;
}

ZTEST(admission, test_unsigned_is_admitted_unattested)
{
	struct akira_admission_ctx ctx = install_ctx();
	struct akira_admission_result res;

	zassert_ok(akira_admission_evaluate(&ctx, &res));
	zassert_false(res.attested);
	zassert_equal(res.cap_clamp, UINT64_MAX);
	zassert_equal(t.early_calls, 1);
	zassert_equal(t.late_calls, 1);
}

ZTEST(admission, test_baseline_attests_oem_signed_package)
{
	struct akira_admission_ctx ctx = install_ctx();
	struct akira_admission_result res;
	static const uint8_t zero[AKIRA_ADMISSION_SIGNER_ID_LEN];

	ctx.package_digest = pkg_digest;
	ctx.sig_ed25519 = pkg_sig;
	ctx.sig_ed25519_len = sizeof(pkg_sig);

	zassert_ok(akira_admission_evaluate(&ctx, &res));
	zassert_true(res.attested, "OEM-signed package must be attested");
	zassert_true(memcmp(res.signer_id, zero, sizeof(zero)) != 0,
		     "the record must name the signing key");
}

ZTEST(admission, test_bad_signature_is_not_attested)
{
	struct akira_admission_ctx ctx = install_ctx();
	struct akira_admission_result res;
	uint8_t sig[64];

	memcpy(sig, pkg_sig, sizeof(sig));
	sig[0] ^= 0x01;
	ctx.package_digest = pkg_digest;
	ctx.sig_ed25519 = sig;
	ctx.sig_ed25519_len = sizeof(sig);

	zassert_ok(akira_admission_evaluate(&ctx, &res),
		   "a bad signature is not trusted, but another provider might be");
	zassert_false(res.attested);

	ctx.sig_ed25519 = pkg_sig;
	ctx.sig_ed25519_len = 63;
	zassert_ok(akira_admission_evaluate(&ctx, &res));
	zassert_false(res.attested, "a truncated signature never attests");
}

ZTEST(admission, test_deny_stops_evaluation)
{
	struct akira_admission_ctx ctx = install_ctx();
	struct akira_admission_result res;

	t.early_verdict = AKIRA_ADMISSION_DENY;
	ctx.package_digest = pkg_digest;
	ctx.sig_ed25519 = pkg_sig;
	ctx.sig_ed25519_len = sizeof(pkg_sig);

	zassert_equal(akira_admission_evaluate(&ctx, &res), -EACCES);
	zassert_false(res.attested, "a refused app is never attested");
	zassert_str_equal(res.reason, "early says no");
	zassert_equal(t.late_calls, 0, "providers after a DENY must not run");
}

ZTEST(admission, test_late_deny_overrides_earlier_attestation)
{
	struct akira_admission_ctx ctx = install_ctx();
	struct akira_admission_result res;

	t.early_verdict = AKIRA_ADMISSION_ALLOW;
	t.early_attest = true;
	t.late_verdict = AKIRA_ADMISSION_DENY;
	zassert_equal(akira_admission_evaluate(&ctx, &res), -EACCES);
	zassert_false(res.attested);
}

ZTEST(admission, test_clamps_and_first_signer)
{
	struct akira_admission_ctx ctx = install_ctx();
	struct akira_admission_result res;
	uint8_t ee[AKIRA_ADMISSION_SIGNER_ID_LEN];

	t.early_verdict = AKIRA_ADMISSION_ALLOW;
	t.early_attest = true;
	t.early_clamp = 0x00ff;
	ctx.package_digest = pkg_digest;
	ctx.sig_ed25519 = pkg_sig;
	ctx.sig_ed25519_len = sizeof(pkg_sig);

	zassert_ok(akira_admission_evaluate(&ctx, &res));
	zassert_true(res.attested);
	zassert_equal(res.cap_clamp, 0x00ff);
	memset(ee, 0xee, sizeof(ee));
	zassert_mem_equal(res.signer_id, ee, sizeof(ee),
			  "the record names the first provider that attested");
}

ZTEST(admission, test_attest_requires_allow)
{
	struct akira_admission_ctx ctx = install_ctx();
	struct akira_admission_result res;

	t.early_verdict = AKIRA_ADMISSION_ABSTAIN;
	t.early_attest = true; /* ignored without ALLOW */
	zassert_ok(akira_admission_evaluate(&ctx, &res));
	zassert_false(res.attested);
}

ZTEST(admission, test_invalid_verdict_fails_closed)
{
	struct akira_admission_ctx ctx = install_ctx();
	struct akira_admission_result res;

	t.early_verdict = 42;
	zassert_equal(akira_admission_evaluate(&ctx, &res), -EACCES);
}

ZTEST(admission, test_load_trusts_record_from_current_key_only)
{
	struct akira_admission_ctx ictx = install_ctx();
	struct akira_admission_result res;
	struct akira_admission_record rec = { 0 };

	ictx.package_digest = pkg_digest;
	ictx.sig_ed25519 = pkg_sig;
	ictx.sig_ed25519_len = sizeof(pkg_sig);
	zassert_ok(akira_admission_evaluate(&ictx, &res));

	rec.attested = 1;
	memcpy(rec.signer_id, res.signer_id, sizeof(rec.signer_id));

	struct akira_admission_ctx lctx = {
		.phase = AKIRA_ADMISSION_LOAD,
		.name = "hello",
		.binary_sha256 = some_hash,
		.record = &rec,
	};

	zassert_ok(akira_admission_evaluate(&lctx, &res));
	zassert_true(res.attested, "record signed by the configured key re-attests");

	rec.signer_id[0] ^= 0xff;
	zassert_ok(akira_admission_evaluate(&lctx, &res));
	zassert_false(res.attested, "a record naming another key is not trusted");

	lctx.record = NULL;
	zassert_ok(akira_admission_evaluate(&lctx, &res));
	zassert_false(res.attested, "no record, no attestation");
}

ZTEST(admission, test_source_names)
{
	zassert_str_equal(akira_app_source_name(AKIRA_APP_SOURCE_FLEET), "fleet");
	zassert_str_equal(akira_app_source_name(AKIRA_APP_SOURCE_BLE), "ble");
	zassert_str_equal(akira_app_source_name(99), "unknown");
}

ZTEST_SUITE(ed25519, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(admission, NULL, NULL, admission_reset, NULL, NULL);

/* ===== Network policy ===== */

static int net_calls;

static int test_net_policy(const struct akira_net_policy_ctx *ctx, void *user)
{
	ARG_UNUSED(user);
	net_calls++;
	if (ctx->op == AKIRA_NET_OP_BIND && ctx->port < 1024) {
		return -EPERM;
	}
	if (ctx->host && strcmp(ctx->host, "evil.example") == 0) {
		return -EACCES;
	}
	return 0;
}
AKIRA_NET_POLICY_DEFINE(test_net, test_net_policy, NULL);

ZTEST(net_policy, test_dispatch)
{
	struct akira_net_policy_ctx ok = {
		.op = AKIRA_NET_OP_CONNECT, .app_name = "a", .host = "api.example", .port = 443,
	};
	struct akira_net_policy_ctx bad = ok;
	struct akira_net_policy_ctx bind = {
		.op = AKIRA_NET_OP_BIND, .app_name = "a", .port = 80,
	};

	bad.host = "evil.example";
	net_calls = 0;
	zassert_ok(akira_net_policy_check(&ok));
	zassert_equal(akira_net_policy_check(&bad), -EACCES);
	zassert_equal(akira_net_policy_check(&bind), -EPERM,
		      "the provider's errno is returned");
	zassert_equal(net_calls, 3);
	zassert_equal(akira_net_policy_check(NULL), -EINVAL);
}

ZTEST_SUITE(net_policy, NULL, NULL, NULL, NULL, NULL);

/* ===== Audit sinks ===== */

#define SINK_CAP 64
static struct akira_audit_record sunk[SINK_CAP];
static int sunk_n;

static void test_sink(const struct akira_audit_record *rec, void *user)
{
	ARG_UNUSED(user);
	zassert_false(k_is_in_isr(), "sinks never run in ISR context");
	if (sunk_n < SINK_CAP) {
		sunk[sunk_n++] = *rec;
	}
}
AKIRA_AUDIT_SINK_DEFINE(test_audit_sink, test_sink, NULL);

static void *audit_setup(void)
{
	sandbox_init();
	return NULL;
}

static void audit_reset(void *fixture)
{
	ARG_UNUSED(fixture);
	sandbox_init(); /* restarts seq at 0 and empties the ring */
	sunk_n = 0;
}

ZTEST(audit_sink, test_records_reach_sinks_in_order)
{
	akira_audit_emit(AKIRA_AUDIT_ADMISSION_DENIED, "app1", 7);
	akira_audit_emit(AKIRA_AUDIT_VENDOR_FIRST + 3, "acme", 9);
	k_sleep(K_MSEC(50));

	zassert_equal(sunk_n, 2);
	zassert_equal(sunk[0].seq, 0);
	zassert_equal(sunk[0].type, AKIRA_AUDIT_ADMISSION_DENIED);
	zassert_equal(sunk[0].detail, 7);
	zassert_str_equal(sunk[0].app_name, "app1");
	zassert_equal(sunk[1].seq, 1);
	zassert_equal(sunk[1].type, AKIRA_AUDIT_VENDOR_FIRST + 3);
}

ZTEST(audit_sink, test_overflow_reports_dropped)
{
	/* The ring holds CONFIG_AKIRA_AUDIT_LOG_SIZE records. Emit more from a
	 * cooperative context so the sink work cannot run in between. */
	const int extra = 5;
	const int total = CONFIG_AKIRA_AUDIT_LOG_SIZE + extra;

	k_sched_lock();
	for (int i = 0; i < total; i++) {
		akira_audit_emit(AKIRA_AUDIT_APP_LOADED, "flood", (uint32_t)i);
	}
	k_sched_unlock();
	k_sleep(K_MSEC(50));

	zassert_equal(sunk_n, 1 + CONFIG_AKIRA_AUDIT_LOG_SIZE, "got %d", sunk_n);
	zassert_equal(sunk[0].type, AKIRA_AUDIT_DROPPED);
	zassert_equal(sunk[0].detail, extra);
	zassert_equal(sunk[1].seq, extra, "delivery resumes at the oldest kept record");
	zassert_equal(sunk[1].detail, extra);
	zassert_equal(sunk[sunk_n - 1].seq, total - 1);
}

ZTEST_SUITE(audit_sink, NULL, audit_setup, audit_reset, NULL, NULL);
