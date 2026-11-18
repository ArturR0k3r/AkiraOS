/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network egress policy dispatch (akira_net_policy.h).
 */

#include <akira_net_policy.h>
#include <akira_audit.h>

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_net_policy, CONFIG_AKIRA_LOG_LEVEL);

/*
 * A refusal is logged and audited, and an app decides how often it asks. So
 * a refusal identical to one reported less than NET_POLICY_REPEAT_MS ago
 * (same app, operation, host and port) is counted instead of reported again;
 * the next report carries the count. Different refusals are always audited.
 * Log lines are also capped overall (NET_POLICY_LOGS_PER_SEC), since an app
 * can vary the host; the audit trail is not, its ring accounts for overflow
 * itself. Providers are asked every time either way.
 */
#define NET_POLICY_REPEAT_MS    1000U
#define NET_POLICY_RECENT       8
#define NET_POLICY_LOGS_PER_SEC 5U

struct net_policy_recent {
	uint32_t key;
	uint32_t at;      /* uptime ms of the last report; 0 = free */
	uint16_t repeats; /* identical refusals since that report */
};

static struct net_policy_recent s_recent[NET_POLICY_RECENT];
static uint32_t s_log_window;
static uint32_t s_log_count;
static uint32_t s_log_skipped;
static struct k_spinlock s_recent_lock;

static uint32_t fnv1a(uint32_t h, const void *data, size_t len)
{
	const uint8_t *p = data;

	for (size_t i = 0; i < len; i++) {
		h = (h ^ p[i]) * 16777619U;
	}
	return h;
}

static uint32_t refusal_key(const struct akira_net_policy_ctx *ctx)
{
	const char *app = ctx->app_name ? ctx->app_name : "";
	const char *host = ctx->host ? ctx->host : "";
	uint8_t op = (uint8_t)ctx->op;
	uint32_t h = 2166136261U;

	h = fnv1a(h, &op, sizeof(op));
	h = fnv1a(h, &ctx->port, sizeof(ctx->port));
	h = fnv1a(h, app, strlen(app) + 1);
	return fnv1a(h, host, strlen(host) + 1);
}

/*
 * Decide whether to report this refusal.
 * @param repeats  out: identical refusals suppressed since the last report.
 * @param log      out: whether the report may also be logged.
 * @return false if it repeats one reported within NET_POLICY_REPEAT_MS.
 */
static bool refusal_report(uint32_t key, uint16_t *repeats, bool *log, uint32_t *skipped)
{
	uint32_t now = k_uptime_get_32();
	struct net_policy_recent *slot = NULL;
	struct net_policy_recent *victim = &s_recent[0];
	bool report = true;
	k_spinlock_key_t k = k_spin_lock(&s_recent_lock);

	for (int i = 0; i < NET_POLICY_RECENT; i++) {
		struct net_policy_recent *e = &s_recent[i];

		if (e->at != 0U && e->key == key) {
			slot = e;
			break;
		}
		if (victim->at != 0U && (e->at == 0U || (now - e->at) > (now - victim->at))) {
			victim = e;
		}
	}

	*repeats = 0;
	if (slot && (now - slot->at) < NET_POLICY_REPEAT_MS) {
		if (slot->repeats < UINT16_MAX) {
			slot->repeats++;
		}
		report = false;
	} else {
		if (slot) {
			*repeats = slot->repeats;
		} else {
			slot = victim;
		}
		slot->key = key;
		slot->at = now ? now : 1U;
		slot->repeats = 0;
	}

	*log = false;
	*skipped = 0;
	if (report) {
		if ((now - s_log_window) >= 1000U) {
			s_log_window = now;
			s_log_count = 0;
		}
		if (s_log_count < NET_POLICY_LOGS_PER_SEC) {
			s_log_count++;
			*log = true;
			*skipped = s_log_skipped;
			s_log_skipped = 0;
		} else {
			s_log_skipped++;
		}
	}
	k_spin_unlock(&s_recent_lock, k);
	return report;
}

static const char *op_name(enum akira_net_op op)
{
	switch (op) {
	case AKIRA_NET_OP_CONNECT:
		return "connect";
	case AKIRA_NET_OP_BIND:
		return "bind";
	case AKIRA_NET_OP_LISTEN:
		return "listen";
	case AKIRA_NET_OP_FETCH_URL:
		return "fetch";
	default:
		return "?";
	}
}

int akira_net_policy_check(const struct akira_net_policy_ctx *ctx)
{
	if (!ctx) {
		return -EINVAL;
	}

	STRUCT_SECTION_FOREACH(akira_net_policy, p) {
		if (!p->fn) {
			continue;
		}
		int ret = p->fn(ctx, p->user_data);

		if (ret != 0) {
			uint16_t repeats;
			uint32_t skipped;
			bool log;

			if (refusal_report(refusal_key(ctx), &repeats, &log, &skipped)) {
				if (log && (repeats != 0U || skipped != 0U)) {
					LOG_WRN("net %s by '%s' to %s:%u denied by policy"
						" (%u identical before, %u others not logged)",
						op_name(ctx->op),
						ctx->app_name ? ctx->app_name : "?",
						ctx->host ? ctx->host : "-", ctx->port,
						repeats, skipped);
				} else if (log) {
					LOG_WRN("net %s by '%s' to %s:%u denied by policy",
						op_name(ctx->op),
						ctx->app_name ? ctx->app_name : "?",
						ctx->host ? ctx->host : "-", ctx->port);
				}
				/* detail = op << 16 | port */
				akira_audit_emit(AKIRA_AUDIT_NET_DENIED, ctx->app_name,
						 ((uint32_t)ctx->op << 16) | ctx->port);
			}
			return ret < 0 ? ret : -EACCES;
		}
	}
	return 0;
}
