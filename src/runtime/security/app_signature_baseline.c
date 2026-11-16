/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * Baseline app signature check: one OEM Ed25519 key (CONFIG_AKIRA_APP_PUBKEY).
 *
 * akira-cli `sign` stores sig.ed25519 = Ed25519(SHA-256(manifest.json ||
 * app binary [|| model.tflite])) in the .akpkg. At install this provider
 * verifies it against the configured key and attests the app. At every load
 * it re-attests apps whose admission record names this key; an app signed
 * with a key that has since been replaced (firmware update) is no longer
 * attested.
 *
 * A signature that does not verify is not a refusal here: another provider
 * (e.g. AkiraPlatform's publisher keys) may trust it. With
 * CONFIG_AKIRA_REQUIRE_SIGNED_APPS=y an app nobody attests is refused by the
 * admission core.
 */

#include <akira_admission.h>
#include <akira_audit.h>
#include <akira_ed25519.h>

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <mbedtls/sha256.h>

LOG_MODULE_REGISTER(akira_app_sig, CONFIG_AKIRA_LOG_LEVEL);

static struct {
	bool loaded;
	bool valid;
	uint8_t pubkey[AKIRA_ED25519_PUBKEY_LEN];
	uint8_t signer_id[AKIRA_ADMISSION_SIGNER_ID_LEN];
} s_key;

/* signer_id = first 16 bytes of SHA-256("akira-ed25519" || pubkey). */
static void compute_signer_id(const uint8_t *pubkey, uint8_t *out)
{
	static const char tag[] = "akira-ed25519";
	uint8_t h[32];
	mbedtls_sha256_context sha;

	mbedtls_sha256_init(&sha);
	mbedtls_sha256_starts(&sha, 0);
	mbedtls_sha256_update(&sha, (const uint8_t *)tag, sizeof(tag) - 1);
	mbedtls_sha256_update(&sha, pubkey, AKIRA_ED25519_PUBKEY_LEN);
	mbedtls_sha256_finish(&sha, h);
	mbedtls_sha256_free(&sha);
	memcpy(out, h, AKIRA_ADMISSION_SIGNER_ID_LEN);
}

static bool baseline_key(void)
{
	if (!s_key.loaded) {
		s_key.loaded = true;
		const char *hex = CONFIG_AKIRA_APP_PUBKEY;

		if (hex[0] == '\0') {
			LOG_INF("App signing: no OEM key (CONFIG_AKIRA_APP_PUBKEY empty)");
		} else if (akira_ed25519_pubkey_from_hex(hex, s_key.pubkey) != 0) {
			LOG_ERR("CONFIG_AKIRA_APP_PUBKEY is not 64 hex digits; ignored");
		} else {
			compute_signer_id(s_key.pubkey, s_key.signer_id);
			s_key.valid = true;
			LOG_INF("App signing: OEM Ed25519 key loaded");
		}
	}
	return s_key.valid;
}

static int baseline_admit(const struct akira_admission_ctx *ctx,
			  struct akira_admission_result *res, void *user)
{
	ARG_UNUSED(user);

	if (!baseline_key()) {
		return AKIRA_ADMISSION_ABSTAIN;
	}

	if (ctx->phase == AKIRA_ADMISSION_LOAD) {
		if (ctx->record && ctx->record->attested &&
		    memcmp(ctx->record->signer_id, s_key.signer_id,
			   AKIRA_ADMISSION_SIGNER_ID_LEN) == 0) {
			res->attested = true;
			return AKIRA_ADMISSION_ALLOW;
		}
		return AKIRA_ADMISSION_ABSTAIN;
	}

	if (!ctx->package_digest || !ctx->sig_ed25519) {
		return AKIRA_ADMISSION_ABSTAIN; /* bare .wasm or unsigned package */
	}
	if (ctx->sig_ed25519_len != AKIRA_ED25519_SIG_LEN) {
		LOG_WRN("'%s': sig.ed25519 is %zu bytes, expected %d", ctx->name,
			ctx->sig_ed25519_len, AKIRA_ED25519_SIG_LEN);
		return AKIRA_ADMISSION_ABSTAIN;
	}

	if (akira_ed25519_verify(ctx->package_digest, 32, ctx->sig_ed25519,
				 s_key.pubkey) != 0) {
		LOG_WRN("'%s': sig.ed25519 does not verify against the OEM key",
			ctx->name);
		akira_audit_emit(AKIRA_AUDIT_SIGNATURE_FAIL, ctx->name, 0);
		return AKIRA_ADMISSION_ABSTAIN;
	}

	LOG_INF("'%s': signed by the OEM key", ctx->name);
	akira_audit_emit(AKIRA_AUDIT_SIGNATURE_OK, ctx->name, 0);
	res->attested = true;
	memcpy(res->signer_id, s_key.signer_id, AKIRA_ADMISSION_SIGNER_ID_LEN);
	return AKIRA_ADMISSION_ALLOW;
}

AKIRA_APP_ADMISSION_DEFINE(akira_baseline_signature, AKIRA_ADMISSION_PRIO_CORE,
			   baseline_admit, NULL);
