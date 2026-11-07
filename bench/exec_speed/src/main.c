/*
 * exec_speed — WebAssembly execution speed: classic interpreter vs. (AOT vs.)
 * native C, for an identical compute kernel (src/kernel.h).
 *
 * Native: the kernel is compiled into the firmware and timed directly with
 * CCOUNT. WASM: the same kernel, compiled to wasm32, is run by WAMR and times
 * itself with the same CCOUNT host native (the few host calls are outside the
 * timed compute loop). Results from both paths must match bit-for-bit — a built
 * in correctness check that they really ran the same algorithm.
 *
 * Prints JSON between <<BENCH_JSON_START>>/<<BENCH_JSON_END>> (collect.py).
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>
#include <string.h>
#ifdef CONFIG_SHARED_MULTI_HEAP
#include <zephyr/multi_heap/shared_multi_heap.h>
#endif

#include <wasm_export.h>
#include "kernel.h"
#include "exec_wasm.h"   /* exec_wasm[], exec_wasm_len */
#ifdef EXEC_SPEED_AOT
#include "exec_aot.h"    /* exec_aot[], exec_aot_len (ESP32-S3 .aot) */
#endif

static inline uint32_t ccount_now(void)
{
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
    uint32_t c; __asm__ volatile("rsr %0, CCOUNT" : "=r"(c) : : "memory"); return c;
#else
    return k_cycle_get_32();
#endif
}
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
#define BENCH_CPU_MHZ 240U
#else
#define BENCH_CPU_MHZ (sys_clock_hw_cycles_per_sec() / 1000000U)
#endif

/* WASM run result, filled by bench_report_exec(). */
static volatile uint32_t g_wasm_cycles;
static volatile uint32_t g_wasm_result;
static volatile int      g_wasm_reported;

/* WAMR installs a console hook; provide a weak no-op for standalone builds
 * (e.g. native_sim) that do not supply one. We print via printk. */
__attribute__((weak)) void __stdout_hook_install(int (*hook)(int)) { (void)hook; }

static int32_t  native_printf(wasm_exec_env_t e, const char *s){ (void)e; if (s) printk("%s", s); return 0; }
static uint32_t native_ccount(wasm_exec_env_t e){ (void)e; return ccount_now(); }
static void     native_pin_core(wasm_exec_env_t e, int32_t c){ (void)e; (void)c; }
static void     native_report_exec(wasm_exec_env_t e, uint32_t cycles, uint32_t result)
{
    (void)e; g_wasm_cycles = cycles; g_wasm_result = result; g_wasm_reported = 1;
}

static NativeSymbol g_natives[] = {
    { "printf_native",    (void *)native_printf,      "($)i", NULL },
    { "bench_ccount_get", (void *)native_ccount,      "()i",  NULL },
    { "bench_pin_core",   (void *)native_pin_core,    "(i)",  NULL },
    { "bench_report_exec",(void *)native_report_exec, "(ii)", NULL },
};

#define WASM_STACK 8192
#define WASM_HEAP  16384

/* Run an embedded module (interp or AOT; WAMR detects from the magic) and return
 * the cycles it reported, or 0 on failure. */
static uint32_t run_module(const uint8_t *bytes, uint32_t len, uint32_t *result_out)
{
    /* The .aot must be staged exactly as production does: into a 16-byte-aligned
     * PSRAM (shared-multi-heap EXTERNAL) buffer. On ESP32-S3 the AOT loader's
     * Xtensa relocations are computed relative to this buffer, and the dual-bus
     * path only works when it lives in the PSRAM region (akira_malloc_buffer is
     * PSRAM-first for the same reason). A DRAM buffer makes l32r offsets/literal
     * slots land out of range / unaligned. Fall back to an aligned static buffer
     * on hosts without PSRAM (native_sim, interp .wasm). */
    uint8_t *buf = NULL;
    bool buf_is_psram = false;
#ifdef CONFIG_SHARED_MULTI_HEAP
    buf = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, 16, len);
    if (buf) buf_is_psram = true;
#endif
    static uint8_t __aligned(16) fallback_buf[4096];
    if (!buf) {
        if (len > sizeof(fallback_buf)) { printk("module too big\n"); return 0; }
        buf = fallback_buf;
    }
    memcpy(buf, bytes, len);   /* loader rewrites in place; copy to staged RAM */

    char err[128] = {0};
    wasm_module_t mod = wasm_runtime_load(buf, len, err, sizeof(err));
    if (!mod) {
        printk("load failed: %s\n", err);
#ifdef CONFIG_SHARED_MULTI_HEAP
        if (buf_is_psram) shared_multi_heap_free(buf);
#endif
        return 0;
    }
    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, WASM_STACK, WASM_HEAP, err, sizeof(err));
    if (!inst) { printk("instantiate failed: %s\n", err); wasm_runtime_unload(mod); return 0; }
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, WASM_STACK);
    wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, "main");
    if (!fn) fn = wasm_runtime_lookup_function(inst, "__main_argc_argv");
    g_wasm_reported = 0;
    uint32_t argv[1] = {0};
    if (fn) wasm_runtime_call_wasm(env, fn, 0, argv);
    if (!g_wasm_reported) printk("module did not report: %s\n", wasm_runtime_get_exception(inst));
    wasm_runtime_destroy_exec_env(env);
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);   /* WAMR held pointers into buf until here */
#ifdef CONFIG_SHARED_MULTI_HEAP
    if (buf_is_psram) shared_multi_heap_free(buf);
#endif
    if (result_out) *result_out = g_wasm_result;
    return g_wasm_cycles;
}

int main(void)
{
    printk("\n==== exec_speed (interpreter vs native) ====\n");
#ifdef CONFIG_SCHED_CPU_MASK
    k_thread_cpu_mask_clear(k_current_get());
    k_thread_cpu_mask_enable(k_current_get(), 1);
#endif

    /* ── Native C ────────────────────────────────────────────────────────── */
    volatile uint32_t warm = compute_kernel(1u, 10000u); (void)warm;
    uint32_t n0 = ccount_now();
    uint32_t nat_res = compute_kernel(EXEC_KERNEL_SEED, EXEC_KERNEL_ITERS);
    uint32_t n1 = ccount_now();
    uint32_t nat_cycles = n1 - n0;

    RuntimeInitArgs init = {0};
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) { printk("wamr init failed\n"); return -1; }
    wasm_runtime_register_natives("env", g_natives, ARRAY_SIZE(g_natives));

    /* ── WASM classic interpreter ────────────────────────────────────────── */
    uint32_t interp_res = 0;
    uint32_t interp_cycles = run_module(exec_wasm, exec_wasm_len, &interp_res);

    uint32_t mhz = BENCH_CPU_MHZ ? BENCH_CPU_MHZ : 1;
    uint64_t ratio_x1000 = nat_cycles ? ((uint64_t)interp_cycles * 1000ULL / nat_cycles) : 0;

#ifdef EXEC_SPEED_AOT
    /* ── WASM AOT (wamrc, native machine code) ───────────────────────────── */
    uint32_t aot_res = 0;
    uint32_t aot_cycles = run_module(exec_aot, exec_aot_len, &aot_res);
    uint64_t aot_vs_native_x1000 = nat_cycles ? ((uint64_t)aot_cycles * 1000ULL / nat_cycles) : 0;
    uint64_t interp_vs_aot_x1000 = aot_cycles ? ((uint64_t)interp_cycles * 1000ULL / aot_cycles) : 0;

    printk("<<BENCH_JSON_START>>\n");
    printk("{\"bench\":\"B7_exec_speed\",\"board\":\"%s\",\"cpu_mhz\":%u,\"iters\":%u,"
           "\"native_cycles\":%u,\"interp_cycles\":%u,\"aot_cycles\":%u,"
           "\"native_us\":%u,\"interp_us\":%u,\"aot_us\":%u,"
           "\"interp_vs_native_x1000\":%llu,\"aot_vs_native_x1000\":%llu,"
           "\"interp_vs_aot_x1000\":%llu,"
           "\"native_result\":%u,\"interp_result\":%u,\"aot_result\":%u,"
           "\"results_match\":%s,"
           "\"mock_mode\":false}\n",
           CONFIG_BOARD, (unsigned)mhz, EXEC_KERNEL_ITERS,
           nat_cycles, interp_cycles, aot_cycles,
           (unsigned)(nat_cycles / mhz), (unsigned)(interp_cycles / mhz),
           (unsigned)(aot_cycles / mhz),
           (unsigned long long)ratio_x1000, (unsigned long long)aot_vs_native_x1000,
           (unsigned long long)interp_vs_aot_x1000,
           nat_res, interp_res, aot_res,
           (nat_res == interp_res && nat_res == aot_res) ? "true" : "false");
    printk("<<BENCH_JSON_END>>\n");
#else
    printk("<<BENCH_JSON_START>>\n");
    printk("{\"bench\":\"B7_exec_speed\",\"board\":\"%s\",\"cpu_mhz\":%u,\"iters\":%u,"
           "\"native_cycles\":%u,\"interp_cycles\":%u,"
           "\"native_us\":%u,\"interp_us\":%u,"
           "\"interp_vs_native_x1000\":%llu,"
           "\"native_result\":%u,\"interp_result\":%u,\"results_match\":%s,"
           "\"mock_mode\":false}\n",
           CONFIG_BOARD, (unsigned)mhz, EXEC_KERNEL_ITERS,
           nat_cycles, interp_cycles,
           (unsigned)(nat_cycles / mhz), (unsigned)(interp_cycles / mhz),
           (unsigned long long)ratio_x1000,
           nat_res, interp_res, (nat_res == interp_res) ? "true" : "false");
    printk("<<BENCH_JSON_END>>\n");
#endif
    printk("==== exec_speed done ====\n");
    return 0;
}
