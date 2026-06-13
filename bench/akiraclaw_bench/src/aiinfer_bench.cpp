/*
 * aiinfer_bench.cpp — minimal aiinfer native API for akiraclaw_bench.
 *
 * Provides akira_native_aiinfer_load/run/unload without the full AkiraOS
 * dependency chain (no security gate, no mem_helper).
 * Arenas are placed in PSRAM (.ext_ram.bss) when CONFIG_AKIRA_PSRAM=y.
 */

#ifdef CONFIG_AKIRA_AIINFER

#include "akira_aiinfer_api.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

LOG_MODULE_DECLARE(akiraclaw_bench, CONFIG_AKIRA_LOG_LEVEL);

#ifndef AKIRA_AIINFER_ARENA_KB
#define AKIRA_AIINFER_ARENA_KB 128
#endif

#ifndef AKIRA_AIINFER_MAX_HANDLES
#define AKIRA_AIINFER_MAX_HANDLES 2
#endif

#define BENCH_ARENA_BYTES (AKIRA_AIINFER_ARENA_KB * 1024U)

#ifdef CONFIG_AKIRA_PSRAM
#define EXT_BSS __attribute__((section(".ext_ram.bss"), aligned(16)))
#else
#define EXT_BSS __attribute__((aligned(16)))
#endif

/* Arenas must be globals to carry section attributes (not struct members). */
static uint8_t g_arena_0[BENCH_ARENA_BYTES] EXT_BSS;
static uint8_t g_arena_1[BENCH_ARENA_BYTES] EXT_BSS;

static uint8_t * const g_arenas[AKIRA_AIINFER_MAX_HANDLES] = {
    g_arena_0,
    g_arena_1,
};

typedef struct {
    bool   used;
    alignas(alignof(tflite::MicroInterpreter))
        uint8_t interp_buf[sizeof(tflite::MicroInterpreter)];
    alignas(alignof(tflite::MicroMutableOpResolver<16>))
        uint8_t resolver_buf[sizeof(tflite::MicroMutableOpResolver<16>)];
    tflite::MicroInterpreter          *interp;
    tflite::MicroMutableOpResolver<16> *resolver;
} bench_infer_slot_t;

static bench_infer_slot_t g_bench_slots[AKIRA_AIINFER_MAX_HANDLES];

static void build_resolver(tflite::MicroMutableOpResolver<16> *r)
{
    r->AddDepthwiseConv2D();
    r->AddConv2D();
    r->AddFullyConnected();
    r->AddSoftmax();
    r->AddReshape();
    r->AddAveragePool2D();
    r->AddMaxPool2D();
    r->AddQuantize();
    r->AddDequantize();
}

int akira_native_aiinfer_load(wasm_exec_env_t exec_env,
                              const uint8_t *model_buf, uint32_t model_len)
{
    (void)exec_env;
    (void)model_len;

    for (int i = 0; i < AKIRA_AIINFER_MAX_HANDLES; i++) {
        bench_infer_slot_t *s = &g_bench_slots[i];
        if (s->used) continue;

        s->resolver = new (s->resolver_buf) tflite::MicroMutableOpResolver<16>();
        build_resolver(s->resolver);

        s->interp = new (s->interp_buf) tflite::MicroInterpreter(
            tflite::GetModel(model_buf), *s->resolver,
            g_arenas[i], BENCH_ARENA_BYTES);

        if (s->interp->AllocateTensors() != kTfLiteOk) {
            LOG_ERR("aiinfer_bench: AllocateTensors failed (slot %d)", i);
            s->interp->~MicroInterpreter();
            s->resolver->~MicroMutableOpResolver<16>();
            return AIINFER_ERR_NOMEM;
        }
        s->used = true;
        LOG_INF("aiinfer_bench: loaded slot %d (%u KB arena)", i,
                AKIRA_AIINFER_ARENA_KB);
        return i;
    }
    return AIINFER_ERR_NOSLOT;
}

int akira_native_aiinfer_run(wasm_exec_env_t exec_env,
                             int handle,
                             const uint8_t *in_buf,  uint32_t in_len,
                             uint8_t       *out_buf, uint32_t out_len)
{
    (void)exec_env;
    if (handle < 0 || handle >= AKIRA_AIINFER_MAX_HANDLES ||
        !g_bench_slots[handle].used) {
        return AIINFER_ERR_INVALID;
    }
    bench_infer_slot_t *s = &g_bench_slots[handle];
    TfLiteTensor *in  = s->interp->input(0);
    TfLiteTensor *out = s->interp->output(0);

    if (!in || !out) return AIINFER_ERR_SHAPE;
    if (in_len < in->bytes) return AIINFER_ERR_SHAPE;

    memcpy(in->data.raw, in_buf, in->bytes);
    if (s->interp->Invoke() != kTfLiteOk) return AIINFER_ERR_INVALID;

    uint32_t copy_len = (out_len < out->bytes) ? out_len : out->bytes;
    memcpy(out_buf, out->data.raw, copy_len);
    return AIINFER_OK;
}

void akira_native_aiinfer_unload(wasm_exec_env_t exec_env, int handle)
{
    (void)exec_env;
    if (handle < 0 || handle >= AKIRA_AIINFER_MAX_HANDLES ||
        !g_bench_slots[handle].used) {
        return;
    }
    bench_infer_slot_t *s = &g_bench_slots[handle];
    s->interp->~MicroInterpreter();
    s->resolver->~MicroMutableOpResolver<16>();
    s->used = false;
    LOG_INF("aiinfer_bench: unloaded slot %d", handle);
}

#endif /* CONFIG_AKIRA_AIINFER */
