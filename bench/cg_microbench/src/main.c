/*
 * cg_microbench — Capability Guard per-call overhead, measured with CCOUNT.
 *
 * Links the REAL guard (src/runtime/security.c: akira_security_check_exec) and
 * runs a tiny WASM module that calls a guarded native (capability GRANTED, so
 * the check takes its allow path) N times and an identical unguarded native N
 * times. The per-call delta is the Capability Guard overhead; the shared loop
 * and WAMR native-dispatch cost cancel. Result printed as one JSON object
 * between <<BENCH_JSON_START>>/<<BENCH_JSON_END>> (same convention as
 * bench/akiraclaw_bench), harvested by scripts/collect.py.
 *
 * No TFLite Micro / AIINFER — this builds anywhere WAMR builds.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>
#include <string.h>

#include <wasm_export.h>
#include "security.h"              /* AKIRA_CHECK_CAP_OR_RETURN, AKIRA_CAP_GPIO_READ */

#include "cg_micro_wasm.h"         /* cg_micro_wasm[], cg_micro_wasm_len */

/* ── CCOUNT (cycle counter) ──────────────────────────────────────────────── */
static inline uint32_t ccount_now(void)
{
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
    uint32_t c;
    __asm__ volatile("rsr %0, CCOUNT" : "=r"(c) : : "memory");
    return c;
#else
    return k_cycle_get_32();  /* native_sim / non-Xtensa fallback */
#endif
}

#ifndef CONFIG_SOC_SERIES_ESP32S3
#define BENCH_CPU_MHZ (sys_clock_hw_cycles_per_sec() / 1000000U)
#else
#define BENCH_CPU_MHZ 240U
#endif

/* ── cap-mask source for the guard ───────────────────────────────────────────
 * The real runtime stores an akira_managed_app_t* in WAMR custom data and reads
 * its cap_mask. We mirror that access pattern exactly with a 1-field struct so
 * the measured path (get_module_inst -> get_custom_data -> field -> AND) matches
 * production. These two symbols are normally provided by akira_runtime.c, which
 * pulls the whole app manager; here the bench provides them. */
typedef struct { uint64_t cap_mask; } cg_app_t;
static cg_app_t g_app = { .cap_mask = AKIRA_CAP_GPIO_READ };  /* capability GRANTED */

uint64_t akira_runtime_get_cap_mask_for_module_inst(wasm_module_inst_t inst)
{
    if (!inst) return 0;
    cg_app_t *a = (cg_app_t *)wasm_runtime_get_custom_data(inst);
    return a ? a->cap_mask : 0;
}

int akira_runtime_get_name_for_module_inst(wasm_module_inst_t inst, char *buf, size_t buflen)
{
    (void)inst;
    if (buf && buflen) strncpy(buf, "cg_micro", buflen);
    return 0;
}

/* sandbox_audit_log is only reached on a DENY (never here); provide a stub so we
 * don't have to link sandbox.c. */
#include <runtime/security/sandbox.h>
void sandbox_audit_log(audit_event_type_t type, const char *app_name, uint32_t detail)
{
    (void)type; (void)app_name; (void)detail;
}

/* WAMR's zephyr platform installs a console hook; provide a weak no-op so the
 * standalone bench links where Zephyr does not supply one (e.g. native_sim).
 * We print via printk directly, so WAMR console routing is not needed. */
__attribute__((weak)) void __stdout_hook_install(int (*hook)(int)) { (void)hook; }

/* ── native functions exposed to the WASM module ─────────────────────────── */
static int32_t native_printf(wasm_exec_env_t env, const char *s)
{
    (void)env; if (s) printk("%s", s); return 0;
}
static uint32_t native_ccount(wasm_exec_env_t env) { (void)env; return ccount_now(); }

static void native_pin_core(wasm_exec_env_t env, int32_t core)
{
    (void)env; (void)core;  /* main thread is already pinned below */
}

/* The measured pair: identical bodies except for the capability check. */
static int32_t native_guarded_noop(wasm_exec_env_t env)
{
    AKIRA_CHECK_CAP_OR_RETURN(env, AKIRA_CAP_GPIO_READ, -1);
    return 0;
}
static int32_t native_unguarded_noop(wasm_exec_env_t env)
{
    (void)env;
    return 0;
}

static void native_report(wasm_exec_env_t env, uint32_t guarded, uint32_t unguarded, uint32_t iters)
{
    (void)env;
    if (iters == 0) iters = 1;
    uint64_t mhz = BENCH_CPU_MHZ ? BENCH_CPU_MHZ : 1;
    /* milli-cycles per call (x1000 for precision) */
    uint64_t g_mc = (uint64_t)guarded * 1000ULL / iters;
    uint64_t u_mc = (uint64_t)unguarded * 1000ULL / iters;
    uint64_t d_mc = (g_mc > u_mc) ? (g_mc - u_mc) : 0;
    /* guard overhead in picoseconds then ns: ns = cycles / MHz * 1000;
     * d_mc is milli-cycles, so ns = d_mc/1000 cycles -> d_mc/1000*1000/mhz = d_mc/mhz */
    uint64_t guard_ns = d_mc / mhz;                    /* ns per guarded call */
    uint64_t guard_ps = (d_mc * 1000ULL) / mhz;        /* ps, for sub-ns precision */

    printk("<<BENCH_JSON_START>>\n");
    printk("{\"bench\":\"B6_capability_guard\","
           "\"board\":\"%s\",\"cpu_mhz\":%u,\"iters\":%u,"
           "\"guarded_total_cycles\":%u,\"unguarded_total_cycles\":%u,"
           "\"guarded_cycles_per_call_x1000\":%llu,"
           "\"unguarded_cycles_per_call_x1000\":%llu,"
           "\"guard_overhead_cycles_x1000\":%llu,"
           "\"guard_overhead_ns\":%llu,\"guard_overhead_ps\":%llu,"
           "\"mock_mode\":false}\n",
           CONFIG_BOARD, (unsigned)mhz, iters,
           guarded, unguarded,
           (unsigned long long)g_mc, (unsigned long long)u_mc,
           (unsigned long long)d_mc,
           (unsigned long long)guard_ns, (unsigned long long)guard_ps);
    printk("<<BENCH_JSON_END>>\n");
}

static NativeSymbol g_natives[] = {
    { "printf_native",   (void *)native_printf,         "($)i", NULL },
    { "bench_ccount_get",(void *)native_ccount,         "()i",  NULL },
    { "bench_pin_core",  (void *)native_pin_core,       "(i)",  NULL },
    { "guarded_noop",    (void *)native_guarded_noop,   "()i",  NULL },
    { "unguarded_noop",  (void *)native_unguarded_noop, "()i",  NULL },
    { "bench_report",    (void *)native_report,         "(iii)",NULL },
};

#define WASM_STACK 8192
#define WASM_HEAP  16384

int main(void)
{
    printk("\n==== cg_microbench (Capability Guard overhead) ====\n");

#ifdef CONFIG_SCHED_CPU_MASK
    k_thread_cpu_mask_clear(k_current_get());
    k_thread_cpu_mask_enable(k_current_get(), 1);
#endif

    RuntimeInitArgs init = {0};
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) { printk("wamr init failed\n"); return -1; }
    if (!wasm_runtime_register_natives("env", g_natives, ARRAY_SIZE(g_natives))) {
        printk("register natives failed\n"); return -1;
    }

    /* The WAMR mini-loader rewrites import strings in place, so the buffer must
     * be writable — the embedded array is const (flash/.rodata). Copy to RAM. */
    static uint8_t wasm_buf[sizeof(cg_micro_wasm)];
    memcpy(wasm_buf, cg_micro_wasm, cg_micro_wasm_len);

    char err[128] = {0};
    wasm_module_t mod = wasm_runtime_load(wasm_buf, cg_micro_wasm_len, err, sizeof(err));
    if (!mod) { printk("load failed: %s\n", err); return -1; }

    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, WASM_STACK, WASM_HEAP, err, sizeof(err));
    if (!inst) { printk("instantiate failed: %s\n", err); return -1; }

    /* Grant the capability by attaching the cap-mask source the guard reads. */
    wasm_runtime_set_custom_data(inst, &g_app);

    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst, WASM_STACK);
    if (!exec_env) { printk("exec_env failed\n"); return -1; }

    wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, "main");
    if (!fn) fn = wasm_runtime_lookup_function(inst, "__main_argc_argv");
    if (!fn) { printk("no main\n"); return -1; }

    uint32_t argv[1] = {0};
    if (!wasm_runtime_call_wasm(exec_env, fn, 0, argv)) {
        printk("call failed: %s\n", wasm_runtime_get_exception(inst));
    }

    printk("==== cg_microbench done ====\n");
    return 0;
}
