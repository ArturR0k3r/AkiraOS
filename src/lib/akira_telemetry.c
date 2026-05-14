/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_telemetry
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_telemetry, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_telemetry.c
 * @brief Structured JSON telemetry sink.
 *
 * Events are serialised as single-line JSON and emitted via Zephyr LOG_INF so
 * they appear in whatever backend is active (UART, RTT, etc.).  The telemetry
 * module does NOT add a separate UART driver — it reuses the existing log backend.
 *
 * Example output:
 *   {"ts":12345,"lvl":"ERR","mod":"ota","msg":"fetch failed","errno":-5}
 *   {"ts":12346,"evt":"app_started","name":"sensor_app","id":1}
 */

#ifdef CONFIG_AKIRA_TELEMETRY

#include <lib/akira_telemetry.h>
#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>

/* Maximum length of a single telemetry line (JSON). */
#define TELEMETRY_LINE_MAX 192

void akira_telemetry_event(const char *module, const char *level,
                            const char *msg, int err_code)
{
    if (!module || !level || !msg) {
        return;
    }

    char buf[TELEMETRY_LINE_MAX];
    int64_t ts = (int64_t)k_uptime_get();

    snprintf(buf, sizeof(buf),
             "{\"ts\":%" PRId64 ",\"lvl\":\"%s\","
             "\"mod\":\"%s\",\"msg\":\"%s\",\"errno\":%d}",
             ts, level, module, msg, err_code);

    LOG_INF("TELEMETRY %s", buf);
}

void akira_telemetry_app_started(const char *app_name, uint32_t instance_id)
{
    if (!app_name) {
        return;
    }

    char buf[TELEMETRY_LINE_MAX];
    int64_t ts = (int64_t)k_uptime_get();

    snprintf(buf, sizeof(buf),
             "{\"ts\":%" PRId64 ",\"evt\":\"app_started\","
             "\"name\":\"%s\",\"id\":%" PRIu32 "}",
             ts, app_name, instance_id);

    LOG_INF("TELEMETRY %s", buf);
}

void akira_telemetry_app_crashed(const char *app_name, uint32_t instance_id,
                                  const char *exception)
{
    if (!app_name) {
        return;
    }

    char buf[TELEMETRY_LINE_MAX];
    int64_t ts = (int64_t)k_uptime_get();

    snprintf(buf, sizeof(buf),
             "{\"ts\":%" PRId64 ",\"evt\":\"app_crashed\","
             "\"name\":\"%s\",\"id\":%" PRIu32 ",\"ex\":\"%s\"}",
             ts, app_name, instance_id,
             exception ? exception : "unknown");

    LOG_ERR("TELEMETRY %s", buf);
}

void akira_telemetry_ota_progress(uint32_t bytes_done, uint32_t bytes_total)
{
    char buf[TELEMETRY_LINE_MAX];
    int64_t ts = (int64_t)k_uptime_get();
    uint8_t pct = (bytes_total > 0)
                  ? (uint8_t)((uint64_t)bytes_done * 100u / bytes_total)
                  : 0u;

    snprintf(buf, sizeof(buf),
             "{\"ts\":%" PRId64 ",\"evt\":\"ota_progress\","
             "\"done\":%" PRIu32 ",\"total\":%" PRIu32 ",\"pct\":%" PRIu8 "}",
             ts, bytes_done, bytes_total, pct);

    LOG_INF("TELEMETRY %s", buf);
}

#endif /* CONFIG_AKIRA_TELEMETRY */
