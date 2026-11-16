/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * App admission (akira_admission.h): runs every registered provider over an
 * install or load and combines their verdicts.
 */

#include <akira_admission.h>
#include <akira_audit.h>

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(akira_admission, CONFIG_AKIRA_LOG_LEVEL);

/* Providers are ordered by prio at each evaluation. There are a handful in any
 * image (core baseline + a product or AkiraPlatform), so a fixed index array
 * and an insertion sort are enough. */
#define ADMISSION_MAX_PROVIDERS 16

const char *akira_app_source_name(enum akira_app_source source)
{
	switch (source) {
	case AKIRA_APP_SOURCE_HTTP:
		return "http";
	case AKIRA_APP_SOURCE_BLE:
		return "ble";
	case AKIRA_APP_SOURCE_USB:
		return "usb";
	case AKIRA_APP_SOURCE_SD:
		return "sd";
	case AKIRA_APP_SOURCE_FIRMWARE:
		return "firmware";
	case AKIRA_APP_SOURCE_FLEET:
		return "fleet";
	default:
		return "unknown";
	}
}

static int sorted_providers(const struct akira_app_admission **out)
{
	int n = 0;

	STRUCT_SECTION_FOREACH(akira_app_admission, p) {
		if (!p->fn) {
			continue;
		}
		if (n == ADMISSION_MAX_PROVIDERS) {
			/* Fail closed: never silently skip a provider. */
			LOG_ERR("more than %d admission providers", ADMISSION_MAX_PROVIDERS);
			return -E2BIG;
		}
		/* Insertion keeps link order among equal priorities. */
		int i = n++;

		while (i > 0 && out[i - 1]->prio > p->prio) {
			out[i] = out[i - 1];
			i--;
		}
		out[i] = p;
	}
	return n;
}

static int deny(const struct akira_admission_ctx *ctx,
		struct akira_admission_result *res, const char *who)
{
	if (!res->reason) {
		res->reason = "refused";
	}
	res->attested = false;
	LOG_WRN("%s of '%s' (%s) refused by %s: %s",
		ctx->phase == AKIRA_ADMISSION_INSTALL ? "install" : "load",
		ctx->name ? ctx->name : "?", akira_app_source_name(ctx->source),
		who, res->reason);
	akira_audit_emit(AKIRA_AUDIT_ADMISSION_DENIED, ctx->name,
			 (uint32_t)ctx->phase);
	return -EACCES;
}

int akira_admission_evaluate(const struct akira_admission_ctx *ctx,
			     struct akira_admission_result *res)
{
	const struct akira_app_admission *providers[ADMISSION_MAX_PROVIDERS];
	bool attested = false;

	if (!ctx || !res || !ctx->binary_sha256) {
		return -EINVAL;
	}

	memset(res, 0, sizeof(*res));
	res->cap_clamp = UINT64_MAX;

	int n = sorted_providers(providers);

	if (n < 0) {
		res->reason = "too many admission providers";
		return deny(ctx, res, "core");
	}

	uint8_t signer[AKIRA_ADMISSION_SIGNER_ID_LEN] = { 0 };

	for (int i = 0; i < n; i++) {
		const struct akira_app_admission *p = providers[i];

		/* attested and signer_id are per provider: an ABSTAIN must not
		 * inherit an earlier provider's flag, and the record names the
		 * first provider that attested (in priority order). */
		res->attested = false;
		memset(res->signer_id, 0, sizeof(res->signer_id));

		int verdict = p->fn(ctx, res, p->user_data);

		if (verdict == AKIRA_ADMISSION_DENY) {
			return deny(ctx, res, p->name);
		}
		if (verdict == AKIRA_ADMISSION_ALLOW && res->attested) {
			if (!attested) {
				memcpy(signer, res->signer_id, sizeof(signer));
			}
			attested = true;
		} else if (verdict != AKIRA_ADMISSION_ALLOW &&
			   verdict != AKIRA_ADMISSION_ABSTAIN) {
			res->reason = "provider returned an invalid verdict";
			return deny(ctx, res, p->name);
		}
	}

	res->attested = attested;
	memcpy(res->signer_id, signer, sizeof(signer));

	if (IS_ENABLED(CONFIG_AKIRA_REQUIRE_SIGNED_APPS) && !attested) {
		res->reason = "not signed by a trusted key (CONFIG_AKIRA_REQUIRE_SIGNED_APPS)";
		return deny(ctx, res, "core");
	}

	LOG_DBG("%s of '%s' admitted (attested=%d clamp=0x%016llx)",
		ctx->phase == AKIRA_ADMISSION_INSTALL ? "install" : "load",
		ctx->name ? ctx->name : "?", attested,
		(unsigned long long)res->cap_clamp);
	return 0;
}
