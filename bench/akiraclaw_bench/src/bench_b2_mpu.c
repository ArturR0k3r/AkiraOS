/*
 * bench_b2_mpu.c — B2: sandbox_exec_begin / sandbox_exec_end isolation cost
 *
 * Measures the overhead of the k_mem_domain add/remove pair that implements
 * per-WASM-app MPU sandboxing.  10 000 iterations, CCOUNT timing.
 *
 * ARM MPU dependency:
 *   CONFIG_AKIRA_SANDBOX_MPU=y depends on CONFIG_ARM_MPU=y, which is only
 *   available on ARM Cortex-M/A targets.  ESP32-S3 (Xtensa LX7) does not
 *   implement this Zephyr MPU backend.  On ESP32-S3 this benchmark reports
 *   mock_mode=true with the reason documented below.
 *
 *   To run B2 with real numbers, target an ARM board (e.g. nrf5340dk or
 *   stm32h7) with CONFIG_AKIRA_SANDBOX_MPU=y.
 *
 * Without MPU (CONFIG_AKIRA_SANDBOX=y but CONFIG_AKIRA_SANDBOX_MPU=n):
 *   sandbox_exec_begin/end still run — they update exec_active and the
 *   watchdog timestamp.  This sub-benchmark measures THAT baseline cost
 *   even on ESP32-S3, and separately reports the MPU increment as mock.
 */

#include "result.h"
#include "timing.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_AKIRA_SANDBOX
#include "runtime/security/sandbox.h"
#include "runtime/security/trust_levels.h"
#endif

LOG_MODULE_DECLARE(akiraclaw_bench, CONFIG_AKIRA_LOG_LEVEL);

#define B2_ITERS 10000

/* Separate ns-resolution stat from timing.h's us stat */
typedef struct {
    uint32_t samples_ns[B2_ITERS];
    int      n;
    uint64_t sum_ns;
    uint32_t max_ns;
} b2_stat_t;

/* 10000 * 4 * 2 = 80 KB — place in PSRAM on ESP32-S3 */
#ifdef CONFIG_AKIRA_PSRAM
#define B2_BSS __attribute__((section(".ext_ram.bss"), aligned(4)))
#else
#define B2_BSS
#endif
static b2_stat_t g_begin_stat B2_BSS;
static b2_stat_t g_end_stat   B2_BSS;

static void b2_stat_reset(b2_stat_t *s) { s->n = 0; s->sum_ns = 0; s->max_ns = 0; }

static void b2_stat_add(b2_stat_t *s, uint32_t cycles)
{
    uint32_t ns = bench_cycles_to_ns(cycles);
    if (s->n < B2_ITERS) s->samples_ns[s->n++] = ns;
    s->sum_ns += ns;
    if (ns > s->max_ns) s->max_ns = ns;
}

static int b2_ns_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t b2_stat_avg(const b2_stat_t *s)
{
    return s->n ? (uint32_t)(s->sum_ns / (uint64_t)s->n) : 0;
}

static uint32_t b2_stat_p99(b2_stat_t *s)
{
    if (s->n < 2) return s->n ? s->samples_ns[0] : 0;
    qsort(s->samples_ns, (size_t)s->n, sizeof(s->samples_ns[0]), b2_ns_cmp);
    int idx = (s->n * 99) / 100;
    if (idx >= s->n) idx = s->n - 1;
    return s->samples_ns[idx];
}

void bench_b2_run(void)
{
    bench_b2_result_t result = {
        .iterations = B2_ITERS,
        .mock_mode  = false,
        .mock_reason = NULL,
    };

#ifndef CONFIG_AKIRA_SANDBOX
    result.mock_mode   = true;
    result.mock_reason = "CONFIG_AKIRA_SANDBOX=n in this build";
    bench_print_b2(&result);
    return;
#else

    /* ── Initialise a scratch sandbox context ────────────────────────── */
    static sandbox_ctx_t ctx;
    sandbox_ctx_init(&ctx, TRUST_LEVEL_USER, 0);

#ifdef CONFIG_AKIRA_SANDBOX_MPU
    /* Provide a dummy WASM heap region for the MPU partition.
     * Must be power-of-two aligned (ARMv7-M MPU constraint). */
    static uint8_t dummy_heap[4096] __aligned(4096);
    sandbox_mpu_configure(&ctx, dummy_heap, sizeof(dummy_heap));
#endif

    /* ── Warm-up (not measured) ──────────────────────────────────────── */
    for (int i = 0; i < 50; i++) {
        sandbox_exec_begin(&ctx);
        sandbox_exec_end(&ctx);
    }

    /* ── Measure sandbox_exec_begin ─────────────────────────────────── */
    b2_stat_reset(&g_begin_stat);
    uint32_t t0, t1;

    for (int i = 0; i < B2_ITERS; i++) {
        BENCH_TS_START(t0);
        sandbox_exec_begin(&ctx);
        BENCH_TS_STOP(t1);
        b2_stat_add(&g_begin_stat, t1 - t0);

        /* Must call end to reset exec_active for the next iteration */
        sandbox_exec_end(&ctx);
    }

    /* ── Measure sandbox_exec_end ───────────────────────────────────── */
    b2_stat_reset(&g_end_stat);

    for (int i = 0; i < B2_ITERS; i++) {
        sandbox_exec_begin(&ctx);

        BENCH_TS_START(t0);
        sandbox_exec_end(&ctx);
        BENCH_TS_STOP(t1);
        b2_stat_add(&g_end_stat, t1 - t0);
    }

    result.begin_avg_ns = b2_stat_avg(&g_begin_stat);
    result.begin_p99_ns = b2_stat_p99(&g_begin_stat);
    result.begin_max_ns = g_begin_stat.max_ns;
    result.end_avg_ns   = b2_stat_avg(&g_end_stat);
    result.end_p99_ns   = b2_stat_p99(&g_end_stat);
    result.end_max_ns   = g_end_stat.max_ns;

#ifndef CONFIG_AKIRA_SANDBOX_MPU
    /* Measured the watchdog-only path.  Separately note MPU not active. */
    result.mock_mode   = true;
    result.mock_reason = "CONFIG_AKIRA_SANDBOX_MPU=n (ARM_MPU unavailable on ESP32-S3); "
                         "values reflect watchdog-timestamp path only, not k_mem_domain cost";
#endif

    bench_print_b2(&result);

    LOG_INF("B2: begin avg=%u p99=%u ns | end avg=%u p99=%u ns",
            result.begin_avg_ns, result.begin_p99_ns,
            result.end_avg_ns,   result.end_p99_ns);
#endif /* CONFIG_AKIRA_SANDBOX */
}
