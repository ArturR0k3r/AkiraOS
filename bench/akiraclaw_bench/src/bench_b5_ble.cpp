/*
 * bench_b5_ble.cpp — B5: inference with BLE advertising active
 *
 * Two sub-runs: BLE off baseline, then BLE on (100 ms adv interval).
 * Inference uses TFLite Micro directly so WASM dispatch is not a
 * confounder — the measurement isolates radio/interrupt coexistence.
 *
 * Requires CONFIG_BT=y and CONFIG_AKIRA_AIINFER=y (prj_ble.conf).
 * mock_mode=true is emitted when either dependency is absent.
 */

#include "result.h"
#include "timing.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#ifdef CONFIG_BT
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#endif

#ifdef CONFIG_AKIRA_AIINFER
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#endif

#include "model_data.h"

LOG_MODULE_DECLARE(akiraclaw_bench, CONFIG_AKIRA_LOG_LEVEL);

#define B5_ITERS      1000
#define ADV_INTERVAL  BT_GAP_ADV_FAST_INT_MIN_2   /* ~100 ms */

static atomic_t g_adv_events;

#ifdef CONFIG_AKIRA_PSRAM
#define EXT_BSS __attribute__((section(".ext_ram.bss"), aligned(16)))
#else
#define EXT_BSS __attribute__((aligned(16)))
#endif

#ifdef CONFIG_AKIRA_AIINFER
#define ARENA_KB 128U
static uint8_t g_b5_arena[ARENA_KB * 1024U] EXT_BSS;
static uint8_t g_b5_resolver_buf[sizeof(tflite::MicroMutableOpResolver<16>)]
    __attribute__((aligned(alignof(tflite::MicroMutableOpResolver<16>)))) EXT_BSS;
static uint8_t g_b5_interp_buf[sizeof(tflite::MicroInterpreter)]
    __attribute__((aligned(alignof(tflite::MicroInterpreter))));

static bool tflm_setup(tflite::MicroMutableOpResolver<16> **res_out,
                       tflite::MicroInterpreter **interp_out)
{
    const tflite::Model *model = tflite::GetModel(hello_world_model);
    if (model->version() != TFLITE_SCHEMA_VERSION) return false;

    auto *resolver = new (g_b5_resolver_buf) tflite::MicroMutableOpResolver<16>();
    resolver->AddDepthwiseConv2D();
    resolver->AddConv2D();
    resolver->AddFullyConnected();
    resolver->AddSoftmax();
    resolver->AddReshape();
    resolver->AddAveragePool2D();
    resolver->AddMaxPool2D();
    resolver->AddQuantize();
    resolver->AddDequantize();

    auto *interp = new (g_b5_interp_buf) tflite::MicroInterpreter(
        model, *resolver, g_b5_arena, sizeof(g_b5_arena));

    if (interp->AllocateTensors() != kTfLiteOk) return false;

    *res_out    = resolver;
    *interp_out = interp;
    return true;
}

static bench_stat_t g_stat;

static void run_loop(tflite::MicroInterpreter *interp, bench_stat_t *stat)
{
    TfLiteTensor *in = interp->input(0);
    bench_stat_reset(stat);
    uint32_t t0, t1;
    for (int i = 0; i < B5_ITERS; i++) {
        float xi = (float)i * (3.14159f / (float)B5_ITERS);
        memcpy(in->data.raw, &xi, sizeof(float));
        BENCH_TS_START(t0);
        interp->Invoke();
        BENCH_TS_STOP(t1);
        bench_stat_add(stat, t1 - t0);
    }
}
#endif /* CONFIG_AKIRA_AIINFER */

extern "C" void bench_b5_run(void)
{
    bench_b5_result_t result = {};
    result.iterations  = B5_ITERS;
    result.mock_mode   = false;
    result.mock_reason = NULL;

#if !defined(CONFIG_BT) || !defined(CONFIG_AKIRA_AIINFER)
    result.mock_mode   = true;
#if !defined(CONFIG_BT)
    result.mock_reason = "CONFIG_BT=n; rebuild with prj_ble.conf";
#else
    result.mock_reason = "CONFIG_AKIRA_AIINFER=n; rebuild with prj_ble.conf";
#endif
    bench_print_b5(&result);
    return;
#else

#ifdef CONFIG_MP_MAX_NUM_CPUS
    k_thread_cpu_mask_clear(k_current_get());
    k_thread_cpu_mask_enable(k_current_get(), 1);
#endif

    tflite::MicroMutableOpResolver<16> *resolver = nullptr;
    tflite::MicroInterpreter *interp = nullptr;

    if (!tflm_setup(&resolver, &interp)) {
        result.mock_mode   = true;
        result.mock_reason = "TFLM setup failed";
        bench_print_b5(&result);
        return;
    }

    /* Warm-up */
    float x_warm = 0.7854f;
    memcpy(interp->input(0)->data.raw, &x_warm, sizeof(float));
    for (int i = 0; i < 5; i++) interp->Invoke();

    /* Sub-run 1: BLE off */
    run_loop(interp, &g_stat);
    result.infer_avg_us_ble_off = bench_stat_avg_us(&g_stat);

    /* Enable BLE */
    int err = bt_enable(NULL);
    if (err && err != -EALREADY) {
        result.mock_mode   = true;
        result.mock_reason = "bt_enable failed";
        bench_print_b5(&result);
        interp->~MicroInterpreter();
        resolver->~MicroMutableOpResolver();
        return;
    }

    atomic_set(&g_adv_events, 0);
    struct bt_le_adv_param param =
        BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONNECTABLE,
                             ADV_INTERVAL, ADV_INTERVAL, NULL);
    err = bt_le_adv_start(&param, NULL, 0, NULL, 0);
    if (err) {
        result.mock_mode   = true;
        result.mock_reason = "bt_le_adv_start failed";
        bench_print_b5(&result);
        interp->~MicroInterpreter();
        resolver->~MicroMutableOpResolver();
        return;
    }

    k_sleep(K_MSEC(200)); /* let controller synchronise */

    /* Sub-run 2: BLE on */
    uint32_t adv_before  = (uint32_t)atomic_get(&g_adv_events);
    int64_t  t_start_ms  = k_uptime_get();

    run_loop(interp, &g_stat);

    int64_t  elapsed_ms   = k_uptime_get() - t_start_ms;
    uint32_t adv_after    = (uint32_t)atomic_get(&g_adv_events);
    uint32_t adv_observed = adv_after - adv_before;
    uint32_t adv_expected = (uint32_t)(elapsed_ms / 100);

    result.total_adv_windows = adv_expected;
    result.missed_adv_events = (adv_observed < adv_expected)
                               ? adv_expected - adv_observed : 0;
    result.infer_avg_us = bench_stat_avg_us(&g_stat);
    result.infer_p99_us = bench_stat_p99_us(&g_stat);
    result.infer_max_us = g_stat.max_us;
    result.ble_active   = true;

    bt_le_adv_stop();

    bench_print_b5(&result);

    LOG_INF("B5: ble_off=%u us | ble_on avg=%u p99=%u max=%u us | missed=%u/%u",
            result.infer_avg_us_ble_off,
            result.infer_avg_us, result.infer_p99_us, result.infer_max_us,
            result.missed_adv_events, result.total_adv_windows);

    interp->~MicroInterpreter();
    resolver->~MicroMutableOpResolver();

#endif /* CONFIG_BT && CONFIG_AKIRA_AIINFER */
}
