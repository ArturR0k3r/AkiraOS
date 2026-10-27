/*
 * timing.h — CCOUNT-based µs / ns timing for AkiraClaw benchmarks
 *
 * On ESP32-S3 (Xtensa LX7 @ 240 MHz) reads the hardware CCOUNT register
 * directly via RSR instruction.  Compiler barriers around every read prevent
 * the compiler from hoisting or sinking the measurement point.
 *
 * Fallback: k_cycle_get_32() + sys_clock_hw_cycles_per_sec() on non-Xtensa.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <zephyr/kernel.h>

/* ── Hardware counter ──────────────────────────────────────────────────── */

#ifdef CONFIG_SOC_SERIES_ESP32S3
#define BENCH_CCOUNT_MHZ 240U
static inline uint32_t bench_ccount(void)
{
    uint32_t c;
    __asm__ volatile("rsr %0, CCOUNT" : "=r"(c) : : "memory");
    return c;
}
#else
/* Generic fallback — less precise but portable */
static inline uint32_t bench_ccount_mhz(void)
{
    return (uint32_t)(sys_clock_hw_cycles_per_sec() / 1000000UL);
}
#define BENCH_CCOUNT_MHZ (bench_ccount_mhz())
static inline uint32_t bench_ccount(void) { return k_cycle_get_32(); }
#endif

/* Compiler barrier: prevents instruction reordering across the fence */
#define BENCH_BARRIER() __asm__ volatile("" : : : "memory")

/* Timestamped read with barriers */
#define BENCH_TS_START(var) do { BENCH_BARRIER(); (var) = bench_ccount(); BENCH_BARRIER(); } while (0)
#define BENCH_TS_STOP(var)  do { BENCH_BARRIER(); (var) = bench_ccount(); BENCH_BARRIER(); } while (0)

/* Cycle → time conversion */
static inline uint32_t bench_cycles_to_us(uint32_t cycles)
{
    return cycles / BENCH_CCOUNT_MHZ;
}
static inline uint32_t bench_cycles_to_ns(uint32_t cycles)
{
    return (uint32_t)(((uint64_t)cycles * 1000ULL) / BENCH_CCOUNT_MHZ);
}

/* ── Statistics ─────────────────────────────────────────────────────────── */

#define BENCH_MAX_ITERS 1000

typedef struct {
    uint32_t samples_us[BENCH_MAX_ITERS];
    int      n;
    uint64_t sum_us;
    uint32_t max_us;
} bench_stat_t;

static inline void bench_stat_reset(bench_stat_t *s)
{
    s->n      = 0;
    s->sum_us = 0;
    s->max_us = 0;
}

static inline void bench_stat_add(bench_stat_t *s, uint32_t cycles)
{
    uint32_t us = bench_cycles_to_us(cycles);
    if (s->n < BENCH_MAX_ITERS) s->samples_us[s->n++] = us;
    s->sum_us += us;
    if (us > s->max_us) s->max_us = us;
}

static int bench_u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static inline uint32_t bench_stat_avg_us(const bench_stat_t *s)
{
    return s->n ? (uint32_t)(s->sum_us / (uint64_t)s->n) : 0;
}

static inline uint32_t bench_stat_p99_us(bench_stat_t *s)
{
    if (s->n < 2) return s->n ? s->samples_us[0] : 0;
    qsort(s->samples_us, (size_t)s->n, sizeof(s->samples_us[0]), bench_u32_cmp);
    /* p99 index: floor(0.99 * n) - 1, clamped */
    int idx = (s->n * 99) / 100;
    if (idx >= s->n) idx = s->n - 1;
    return s->samples_us[idx];
}

/* ── Loop overhead calibration ──────────────────────────────────────────── */

/* Measure the bare cost of BENCH_TS_START + BENCH_TS_STOP with no work.
 * Returns the average overhead in microseconds over @n warmup iterations.
 * Subtract this from reported values in the paper. */
static inline uint32_t bench_loop_overhead_us(int n)
{
    uint32_t t0, t1;
    uint64_t acc = 0;
    for (int i = 0; i < n; i++) {
        BENCH_TS_START(t0);
        BENCH_TS_STOP(t1);
        acc += t1 - t0;
    }
    return bench_cycles_to_us((uint32_t)(acc / (uint64_t)n));
}
