/* WASM side of exec_speed: runs the shared kernel, reports cycles to the host.
 *
 * The module is deliberately given a data section, linear-memory (bulk-memory)
 * activity, and several real functions so wamrc lays it out like a normal app
 * (leading .literal island + l32r), which the ESP32-S3 dual-bus AOT loader
 * relocates correctly. A single trivially-inlined function produced a
 * degenerate .aot that failed to relocate. None of this runs inside the timed
 * region — only compute_kernel() is measured, identical to the native build. */
#include "kernel.h"
extern unsigned int bench_ccount_get(void);
extern void         bench_pin_core(int core);
extern void         bench_report_exec(unsigned int cycles, unsigned int result);

/* A real data/memory footprint (forces a data section + bulk-memory ops). */
#define SCRATCH_N 256
static uint32_t g_scratch[SCRATCH_N];

static void scratch_init(uint32_t seed)
{
    for (int i = 0; i < SCRATCH_N; i++)
        g_scratch[i] = seed + (uint32_t)i * 2654435761u;
}

static uint32_t scratch_mix(void)
{
    uint32_t acc = 0;
    for (int i = 0; i < SCRATCH_N; i++)
        acc ^= g_scratch[i] + (g_scratch[(i + 1) % SCRATCH_N] << 1);
    return acc;
}

/* Keep the compiler from folding everything into main: give it externally
 * visible work and a couple of indirect-ish call sites. */
__attribute__((noinline)) static uint32_t run_kernel(uint32_t seed, uint32_t iters)
{
    return compute_kernel(seed, iters);
}

int main(void)
{
    bench_pin_core(1);

    /* Non-timed warmup + memory activity (data section, bulk memory). */
    scratch_init(EXEC_KERNEL_SEED);
    volatile uint32_t pre = scratch_mix();
    volatile uint32_t warm = run_kernel(1u, 10000u);  /* warm icache */
    (void)pre; (void)warm;

    unsigned int t0 = bench_ccount_get();
    uint32_t res = run_kernel(EXEC_KERNEL_SEED, EXEC_KERNEL_ITERS);
    unsigned int t1 = bench_ccount_get();

    bench_report_exec(t1 - t0, res);
    return 0;
}
