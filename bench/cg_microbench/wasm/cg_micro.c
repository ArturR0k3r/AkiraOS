/*
 * cg_micro.c — Capability Guard micro-benchmark WASM module.
 *
 * Times N calls to a guarded native (full AKIRA_CHECK_CAP_OR_RETURN path, the
 * capability GRANTED so the check passes) vs. N calls to an identical unguarded
 * native. The per-call difference is the Capability Guard overhead; the shared
 * loop + WAMR native-dispatch cost cancels in the delta. Results go back to the
 * host via bench_report(); the host prints the JSON.
 */
extern unsigned int bench_ccount_get(void);
extern int          guarded_noop(void);
extern int          unguarded_noop(void);
extern void         bench_pin_core(int core);
extern void         bench_report(unsigned int guarded_cycles,
                                 unsigned int unguarded_cycles,
                                 unsigned int iters);

#define ITERS 200000u

int main(void)
{
    bench_pin_core(1);

    volatile int sink = 0;

    /* Warm up icache / WAMR dispatch paths (not timed). */
    for (unsigned int i = 0; i < 2000u; i++) {
        sink += guarded_noop();
        sink += unguarded_noop();
    }

    unsigned int t0 = bench_ccount_get();
    for (unsigned int i = 0; i < ITERS; i++) sink += guarded_noop();
    unsigned int t1 = bench_ccount_get();
    for (unsigned int i = 0; i < ITERS; i++) sink += unguarded_noop();
    unsigned int t2 = bench_ccount_get();

    bench_report(t1 - t0, t2 - t1, ITERS);
    (void)sink;
    return 0;
}
