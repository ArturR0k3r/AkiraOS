/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network egress policy dispatch (akira_net_policy.h).
 */

#include <akira_net_policy.h>
#include <akira_audit.h>

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_net_policy, CONFIG_AKIRA_LOG_LEVEL);

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
			LOG_WRN("net %s by '%s' to %s:%u denied by policy",
				op_name(ctx->op),
				ctx->app_name ? ctx->app_name : "?",
				ctx->host ? ctx->host : "-", ctx->port);
			/* detail = op << 16 | port */
			akira_audit_emit(AKIRA_AUDIT_NET_DENIED, ctx->app_name,
					 ((uint32_t)ctx->op << 16) | ctx->port);
			return ret < 0 ? ret : -EACCES;
		}
	}
	return 0;
}
