/*
 * bench_b3_audit.c — B3: HMAC audit log write overhead
 *
 * Triggers AUDIT_EVENT_CAPABILITY_DENIED 10 000 times and measures the
 * latency from violation detected (BENCH_TS_START) to ring-buffer write
 * complete (BENCH_TS_STOP).
 *
 * Two sub-measurements:
 *   hmac_enabled=true   — CONFIG_AKIRA_AUDIT_LOG_HMAC=y (prj_hmac.conf)
 *   hmac_enabled=false  — CONFIG_AKIRA_AUDIT_LOG_HMAC=n (baseline)
 *
 * The benchmark can only measure the build it was compiled with.  Run once
 * with each prj.conf and compare audit_write_avg_ns.
 *
 * mock_mode=true is never set here — sandbox_audit_log is always available
 * when CONFIG_AKIRA_SECURITY_AUDIT=y (selected by CAPABILITY_SYSTEM).
 */

#include "result.h"
#include "timing.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_AKIRA_SECURITY_AUDIT
#include "runtime/security/sandbox.h"
#endif

LOG_MODULE_DECLARE(akiraclaw_bench, CONFIG_AKIRA_LOG_LEVEL);

#define B3_ITERS 1000

typedef struct {
    uint32_t samples_ns[B3_ITERS];
    int      n;
    uint64_t sum_ns;
    uint32_t max_ns;
} b3_stat_t;

#ifdef CONFIG_AKIRA_PSRAM
#define B3_BSS __attribute__((section(".ext_ram.bss"), aligned(4)))
#else
#define B3_BSS
#endif
static b3_stat_t g_stat B3_BSS;

static void b3_stat_reset(b3_stat_t *s) { s->n = 0; s->sum_ns = 0; s->max_ns = 0; }

static void b3_stat_add(b3_stat_t *s, uint32_t cycles)
{
    uint32_t ns = bench_cycles_to_ns(cycles);
    if (s->n < B3_ITERS) s->samples_ns[s->n++] = ns;
    s->sum_ns += ns;
    if (ns > s->max_ns) s->max_ns = ns;
}

static int b3_ns_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t b3_stat_avg(const b3_stat_t *s)
{
    return s->n ? (uint32_t)(s->sum_ns / (uint64_t)s->n) : 0;
}

static uint32_t b3_stat_p99(b3_stat_t *s)
{
    if (s->n < 2) return s->n ? s->samples_ns[0] : 0;
    qsort(s->samples_ns, (size_t)s->n, sizeof(s->samples_ns[0]), b3_ns_cmp);
    int idx = (s->n * 99) / 100;
    if (idx >= s->n) idx = s->n - 1;
    return s->samples_ns[idx];
}

void bench_b3_run(void)
{
    bench_b3_result_t result = {
        .iterations   = B3_ITERS,
        .mock_mode    = false,
        .mock_reason  = NULL,
    };

#ifndef CONFIG_AKIRA_SECURITY_AUDIT
    result.mock_mode   = true;
    result.mock_reason = "CONFIG_AKIRA_SECURITY_AUDIT=n; rebuild with prj_hmac.conf or prj.conf";
    bench_print_b3(&result);
    return;
#else

    /* Disable the Zephyr logger inside the measurement loop: it adds
     * non-deterministic RTT/UART latency.  LOG_MODE_DEFERRED must be set
     * in prj.conf; the backend flushes outside the measured region. */

    /* ── Warm-up ──────────────────────────────────────────────────────── */
    for (int i = 0; i < 50; i++) {
        sandbox_audit_log(AUDIT_EVENT_CAPABILITY_DENIED, "bench_warmup", 0xDEAD);
    }

    /* ── Hot loop ────────────────────────────────────────────────────── */
    b3_stat_reset(&g_stat);
    uint32_t t0, t1;

    for (int i = 0; i < B3_ITERS; i++) {
        /* Vary detail field to prevent the compiler from caching the call */
        uint32_t detail = (uint32_t)i;

        BENCH_TS_START(t0);
        sandbox_audit_log(AUDIT_EVENT_CAPABILITY_DENIED, "akiraclaw_bench", detail);
        BENCH_TS_STOP(t1);

        b3_stat_add(&g_stat, t1 - t0);
    }

    result.audit_write_avg_ns = b3_stat_avg(&g_stat);
    result.audit_write_p99_ns = b3_stat_p99(&g_stat);
    result.audit_write_max_ns = g_stat.max_ns;

    /* audit_nohmac_avg_ns: only meaningful when comparing across two builds.
     * In the HMAC build, set to 0 (the no-HMAC build populates it). */
#ifdef CONFIG_AKIRA_AUDIT_LOG_HMAC
    result.hmac_enabled      = true;
    result.audit_nohmac_avg_ns = 0; /* populate from non-HMAC build run */
#else
    result.hmac_enabled        = false;
    result.audit_nohmac_avg_ns = result.audit_write_avg_ns;
#endif

    bench_print_b3(&result);

    LOG_INF("B3: audit_write avg=%u p99=%u max=%u ns (HMAC=%s)",
            result.audit_write_avg_ns, result.audit_write_p99_ns,
            result.audit_write_max_ns,
            result.hmac_enabled ? "on" : "off");
#endif /* CONFIG_AKIRA_SECURITY_AUDIT */
}
