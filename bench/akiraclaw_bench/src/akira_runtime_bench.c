/*
 * akira_runtime_bench.c — minimal WAMR wrapper for akiraclaw_bench.
 *
 * Provides akira_runtime_{set_exit_callback,load_wasm,start,stop} without
 * pulling in the full AkiraOS runtime dependency chain (manifest_parser,
 * fs_manager, app_signing, etc.).  Intended ONLY for the benchmark app.
 */

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#include <wasm_export.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef CONFIG_AKIRA_AIINFER
#include "akira_aiinfer_api.h"
#endif

#include <zephyr/sys/printk.h>

LOG_MODULE_DECLARE(akiraclaw_bench, CONFIG_AKIRA_LOG_LEVEL);

/* ── Utility natives required by the akiraclaw_bench WASM module ──── */

static int32_t native_printf_native(wasm_exec_env_t env, const char *str)
{
    (void)env;
    if (!str) return -1;
    printk("%s", str);
    return (int32_t)strlen(str);
}

static int32_t native_strlen(wasm_exec_env_t env, const char *str)
{
    (void)env;
    if (!str) return 0;
    return (int32_t)strlen(str);
}

typedef void (*akira_runtime_exit_cb_t)(int slot, int exit_code);

#define BENCH_MAX_SLOTS  2
#define BENCH_STACK_SIZE 4096
#define BENCH_WASM_STACK 4096
#define BENCH_WASM_HEAP  32768

typedef struct {
    bool            used;
    wasm_module_t   module;
    wasm_module_inst_t instance;
    wasm_exec_env_t exec_env;
    struct k_thread thread;
    K_KERNEL_STACK_MEMBER(stack, BENCH_STACK_SIZE);
} bench_slot_t;

static bench_slot_t g_slots[BENCH_MAX_SLOTS];
static akira_runtime_exit_cb_t g_exit_cb;
static bool g_wamr_inited;

static NativeSymbol g_bench_util_natives[] = {
    {"printf_native", (void *)native_printf_native, "($)i", NULL},
    {"strlen",        (void *)native_strlen,        "($)i", NULL},
};

#ifdef CONFIG_AKIRA_AIINFER
static NativeSymbol g_aiinfer_natives[] = {
    {"aiinfer_load",   (void *)akira_native_aiinfer_load,   "(*~)i",    NULL},
    {"aiinfer_run",    (void *)akira_native_aiinfer_run,    "(i*~*~)i", NULL},
    {"aiinfer_unload", (void *)akira_native_aiinfer_unload, "(i)",      NULL},
};
#endif

static int bench_wamr_init(void)
{
    RuntimeInitArgs init = {0};
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) {
        LOG_ERR("bench_runtime: wasm_runtime_full_init failed");
        return -EIO;
    }

    if (!wasm_runtime_register_natives("env", g_bench_util_natives,
                                       ARRAY_SIZE(g_bench_util_natives))) {
        LOG_ERR("bench_runtime: util native registration failed");
        return -EIO;
    }

#ifdef CONFIG_AKIRA_AIINFER
    if (!wasm_runtime_register_natives("env", g_aiinfer_natives,
                                       ARRAY_SIZE(g_aiinfer_natives))) {
        LOG_ERR("bench_runtime: aiinfer native registration failed");
        return -EIO;
    }
    LOG_DBG("bench_runtime: aiinfer natives registered");
#endif

    g_wamr_inited = true;
    LOG_DBG("bench_runtime: WAMR initialized");
    return 0;
}
SYS_INIT(bench_wamr_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

int akira_runtime_init(void)
{
    return g_wamr_inited ? 0 : bench_wamr_init();
}

void akira_runtime_set_exit_callback(akira_runtime_exit_cb_t cb)
{
    g_exit_cb = cb;
}

static void bench_wasm_thread(void *arg1, void *arg2, void *arg3)
{
    int slot = (int)(intptr_t)arg1;
    (void)arg2;
    (void)arg3;
    bench_slot_t *s = &g_slots[slot];
    char err[128] = {0};

    s->instance = wasm_runtime_instantiate(s->module,
                                           BENCH_WASM_STACK, BENCH_WASM_HEAP,
                                           err, sizeof(err));
    if (!s->instance) {
        LOG_ERR("bench_runtime: instantiate failed: %s", err);
        if (g_exit_cb) {
            g_exit_cb(slot, -1);
        }
        return;
    }

    s->exec_env = wasm_runtime_create_exec_env(s->instance, BENCH_WASM_STACK);
    if (!s->exec_env) {
        LOG_ERR("bench_runtime: create_exec_env failed");
        wasm_runtime_deinstantiate(s->instance);
        s->instance = NULL;
        if (g_exit_cb) {
            g_exit_cb(slot, -1);
        }
        return;
    }

    /* Try WASI-style entry first, fall back to bare main */
    wasm_function_inst_t fn =
        wasm_runtime_lookup_function(s->instance, "__main_argc_argv");
    if (!fn) {
        fn = wasm_runtime_lookup_function(s->instance, "main");
    }

    int exit_code = 0;
    if (fn) {
        uint32_t args[2] = {0, 0};
        if (!wasm_runtime_call_wasm(s->exec_env, fn, 2, args)) {
            LOG_WRN("bench_runtime: wasm call returned non-zero");
            exit_code = 1;
        }
    } else {
        LOG_WRN("bench_runtime: no main() found in WASM module");
    }

    if (g_exit_cb) {
        g_exit_cb(slot, exit_code);
    }
}

int akira_runtime_load_wasm(const uint8_t *buffer, uint32_t size)
{
    if (!g_wamr_inited) {
        LOG_ERR("bench_runtime: WAMR not initialized");
        return -1;
    }

    for (int i = 0; i < BENCH_MAX_SLOTS; i++) {
        if (!g_slots[i].used) {
            char err[128] = {0};
            /* Cast away const: WAMR takes uint8_t* but does not modify buffer */
            g_slots[i].module = wasm_runtime_load(
                (uint8_t *)(uintptr_t)buffer, size, err, sizeof(err));
            if (!g_slots[i].module) {
                LOG_ERR("bench_runtime: load failed: %s", err);
                return -1;
            }
            g_slots[i].used     = true;
            g_slots[i].instance = NULL;
            g_slots[i].exec_env = NULL;
            return i;
        }
    }
    LOG_ERR("bench_runtime: no free slots");
    return -1;
}

int akira_runtime_start(int instance_id)
{
    if (instance_id < 0 || instance_id >= BENCH_MAX_SLOTS ||
        !g_slots[instance_id].used) {
        return -1;
    }
    k_thread_create(
        &g_slots[instance_id].thread,
        g_slots[instance_id].stack,
        K_KERNEL_STACK_SIZEOF(g_slots[instance_id].stack),
        bench_wasm_thread,
        (void *)(intptr_t)instance_id, NULL, NULL,
        5, 0, K_NO_WAIT);
    return 0;
}

int akira_runtime_stop(int instance_id)
{
    if (instance_id < 0 || instance_id >= BENCH_MAX_SLOTS ||
        !g_slots[instance_id].used) {
        return -1;
    }
    bench_slot_t *s = &g_slots[instance_id];

    if (s->exec_env) {
        wasm_runtime_destroy_exec_env(s->exec_env);
        s->exec_env = NULL;
    }
    if (s->instance) {
        wasm_runtime_deinstantiate(s->instance);
        s->instance = NULL;
    }
    if (s->module) {
        wasm_runtime_unload(s->module);
        s->module = NULL;
    }
    s->used = false;
    return 0;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
