/*
 * bench_b1_native.cpp — B1 baseline: TFLite Micro called directly from C++
 *
 * Bypasses the AkiraOS WASM wrapper, WAMR dispatch, and capability gate
 * entirely.  Mirrors the setup in akira_aiinfer_api.cpp so the comparison
 * is apples-to-apples: same ops, same arena size, same PSRAM placement.
 *
 * Timing: CCOUNT register (Xtensa @ 240 MHz).  Compiler barriers prevent
 * the compiler from moving the timestamp reads out of the measured region.
 * Loop overhead is calibrated and subtracted before reporting.
 */

#include "bench_b1.h"
#include "timing.h"
#include "result.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

LOG_MODULE_DECLARE(akiraclaw_bench, CONFIG_AKIRA_LOG_LEVEL);

/* ── Model embedded from AkiraSDK (3164 bytes, hello_world_float_sine) ─── */
/* model_data.h is found via the CMakeLists include path for aiinfer_test/ */
#include "model_data.h"

/* ── Tensor arena — PSRAM on ESP32-S3, SRAM on other targets ───────────── */
#ifdef CONFIG_AKIRA_PSRAM
#define EXT_BSS __attribute__((section(".ext_ram.bss"), aligned(16)))
#else
#define EXT_BSS __attribute__((aligned(16)))
#endif

#define ARENA_KB 128U
static uint8_t g_arena[ARENA_KB * 1024U] EXT_BSS;

/* ── TFLM op resolver — same set as akira_aiinfer_api.cpp ─────────────── */
static tflite::MicroMutableOpResolver<16> g_resolver;

static bool g_resolver_built = false;

static void build_resolver(void)
{
    if (g_resolver_built) return;
    g_resolver.AddDepthwiseConv2D();
    g_resolver.AddConv2D();
    g_resolver.AddFullyConnected();
    g_resolver.AddSoftmax();
    g_resolver.AddReshape();
    g_resolver.AddAveragePool2D();
    g_resolver.AddMaxPool2D();
    g_resolver.AddQuantize();
    g_resolver.AddDequantize();
    g_resolver_built = true;
}

/* ── Persistent samples buffer — static to avoid large stack frame ──────── */
static bench_stat_t g_stat;

extern "C" void bench_b1_native_run(void)
{
    bench_b1_result_t result = {
        .config            = "native_tflm",
        .iterations        = BENCH_MAX_ITERS,
        .model_size_bytes  = hello_world_model_len,
        .ble_active        = false,
        .mock_mode         = false,
        .mock_reason       = NULL,
    };

    /* Calibrate loop overhead (50 warm-up iterations) */
    result.loop_overhead_us = bench_loop_overhead_us(50);

    /* ── Load phase ───────────────────────────────────────────────────── */
    build_resolver();
    const tflite::Model *model = tflite::GetModel(hello_world_model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        LOG_ERR("B1/native: schema version mismatch");
        result.mock_mode   = true;
        result.mock_reason = "model schema version mismatch";
        bench_print_b1(&result);
        return;
    }

    uint32_t t0, t1;

    /* Construct interpreter in stack-scoped storage so it is destroyed
     * before we print results.  Placement-new into a static buffer avoids
     * heap operator new (not available in all Zephyr configs). */
    static uint8_t g_interp_buf[sizeof(tflite::MicroInterpreter)]
        __attribute__((aligned(alignof(tflite::MicroInterpreter))));

    BENCH_TS_START(t0);
    tflite::MicroInterpreter *interp =
        new (g_interp_buf) tflite::MicroInterpreter(
            model, g_resolver, g_arena, sizeof(g_arena));
    if (interp->AllocateTensors() != kTfLiteOk) {
        BENCH_TS_STOP(t1);
        LOG_ERR("B1/native: AllocateTensors failed");
        result.mock_mode   = true;
        result.mock_reason = "AllocateTensors failed — arena too small?";
        bench_print_b1(&result);
        return;
    }
    BENCH_TS_STOP(t1);
    result.load_us = bench_cycles_to_us(t1 - t0);

    TfLiteTensor *input  = interp->input(0);
    TfLiteTensor *output = interp->output(0);

    if (!input || !output || input->bytes != sizeof(float)) {
        LOG_ERR("B1/native: unexpected tensor shape");
        result.mock_mode   = true;
        result.mock_reason = "unexpected input/output tensor shape";
        bench_print_b1(&result);
        return;
    }

    /* ── Warm-up (not measured) ─────────────────────────────────────── */
    float x = 0.7854f; /* π/4 */
    for (int i = 0; i < 5; i++) {
        memcpy(input->data.raw, &x, sizeof(float));
        interp->Invoke();
    }

    /* ── Hot-loop: 1000 iterations ───────────────────────────────────── */
    bench_stat_reset(&g_stat);

    for (int i = 0; i < BENCH_MAX_ITERS; i++) {
        /* Rotate input across [0, π] to defeat constant-folding */
        float xi = (float)i * (3.14159f / BENCH_MAX_ITERS);
        memcpy(input->data.raw, &xi, sizeof(float));

        BENCH_TS_START(t0);
        interp->Invoke();
        BENCH_TS_STOP(t1);

        bench_stat_add(&g_stat, t1 - t0);
    }

    result.run_avg_us = bench_stat_avg_us(&g_stat);
    result.run_p99_us = bench_stat_p99_us(&g_stat); /* sorts g_stat.samples_us */
    result.run_max_us = g_stat.max_us;

    /* Subtract loop overhead from average (p99/max left as hardware truth) */
    if (result.run_avg_us > result.loop_overhead_us)
        result.run_avg_us -= result.loop_overhead_us;

    /* ── Unload phase ──────────────────────────────────────────────────── */
    BENCH_TS_START(t0);
    interp->~MicroInterpreter();
    BENCH_TS_STOP(t1);
    result.unload_us = bench_cycles_to_us(t1 - t0);

    bench_print_b1(&result);
    LOG_INF("B1/native_tflm done: avg=%u p99=%u max=%u us",
            result.run_avg_us, result.run_p99_us, result.run_max_us);
}
