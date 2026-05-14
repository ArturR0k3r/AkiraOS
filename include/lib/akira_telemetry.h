/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_telemetry.h
 * @brief Structured JSON telemetry over RTT or UART.
 *
 * When CONFIG_AKIRA_TELEMETRY=n all macros compile to nothing (zero overhead).
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_TELEMETRY_H
#define AKIRA_TELEMETRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AKIRA_TELEMETRY

/**
 * @brief Emit a generic structured event.
 * Output: {"ts":<ms>,"lvl":"<level>","mod":"<module>","msg":"<msg>","errno":<err>}
 */
void akira_telemetry_event(const char *module, const char *level,
                            const char *msg, int err_code);

/** App lifecycle: started */
void akira_telemetry_app_started(const char *app_name, uint32_t instance_id);

/** App lifecycle: crashed */
void akira_telemetry_app_crashed(const char *app_name, uint32_t instance_id,
                                  const char *exception);

/** OTA download progress */
void akira_telemetry_ota_progress(uint32_t bytes_done, uint32_t bytes_total);

#else /* CONFIG_AKIRA_TELEMETRY not set — zero-cost stubs */

static inline void akira_telemetry_event(const char *m, const char *l,
                                          const char *s, int e)
{
    (void)m; (void)l; (void)s; (void)e;
}

static inline void akira_telemetry_app_started(const char *n, uint32_t id)
{
    (void)n; (void)id;
}

static inline void akira_telemetry_app_crashed(const char *n, uint32_t id,
                                                const char *ex)
{
    (void)n; (void)id; (void)ex;
}

static inline void akira_telemetry_ota_progress(uint32_t d, uint32_t t)
{
    (void)d; (void)t;
}

#endif /* CONFIG_AKIRA_TELEMETRY */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_TELEMETRY_H */
