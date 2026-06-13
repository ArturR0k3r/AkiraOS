/*
 * main.c — akiraclaw_bench orchestrator
 *
 * Runs B1-B5 in sequence on boot.  Each sub-benchmark prints one or more
 * JSON objects delimited by <<BENCH_JSON_START>> / <<BENCH_JSON_END>>.
 * The host-side scripts/collect.py extracts these from the serial log.
 *
 * Build variants (select via CMake -DCONF_FILE=):
 *   prj.conf          — baseline: B1 native_tflm + wasm_claw, B2 watchdog, B3 no-HMAC, B4, B5 no-BLE
 *   prj_hmac.conf     — B3 with HMAC enabled
 *   prj_noguard.conf  — B1 wasm_noguard (CONFIG_AKIRA_CAPABILITY_SYSTEM=n)
 *   prj_ble.conf      — B5 with BLE advertising active
 *
 * Thread pinning:
 *   Main benchmark thread is pinned to Core 1 immediately at entry.
 *   This matches the SMP/AMP paper baseline (DOI 10.13140/RG.2.2.19723.25124).
 *   The WASM bench module also calls bench_pin_core(1) on startup.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include "bench_b1.h"
#include "result.h"
#include "timing.h"

LOG_MODULE_REGISTER(akiraclaw_bench, CONFIG_AKIRA_LOG_LEVEL);

/* Forward declarations for B2-B5 (defined in separate .c files) */
void bench_b2_run(void);
void bench_b3_run(void);
void bench_b4_run(void);
void bench_b5_run(void);

/* ── Runtime init barrier: wait for AkiraOS runtime to be ready ─────── */
#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include "runtime/akira_runtime.h"
/* The runtime initialises as a SYS_INIT at APPLICATION level.
 * We yield briefly to ensure it finishes before loading modules. */
static void wait_for_runtime(void)
{
    /* Probe: try to get runtime status.  100 ms is conservative. */
    k_sleep(K_MSEC(100));
}
#else
static void wait_for_runtime(void) {}
#endif

int main(void)
{
    printk("\n");
    printk("========================================\n");
    printk("  akiraclaw_bench v1.0  AkiraOS 4.3.0  \n");
    printk("  platform: esp32s3_akiraconsole        \n");
    printk("========================================\n\n");

    /* ── Pin this thread to Core 1 ──────────────────────────────────── */
#ifdef CONFIG_MP_MAX_NUM_CPUS
    k_thread_cpu_mask_clear(k_current_get());
    k_thread_cpu_mask_enable(k_current_get(), 1);
    LOG_INF("Benchmark thread pinned to Core 1");
#endif

    /* ── B4: memory breakdown (compile-time, run first for quick sanity) */
    LOG_INF("--- B4: memory breakdown ---");
    bench_b4_run();
    log_panic();           /* flush any deferred log messages now */
    k_msleep(200);         /* let UART TX FIFO drain before next section */

    /* ── Wait for WASM runtime ──────────────────────────────────────── */
    wait_for_runtime();

    /* ── B1 native_tflm: TFLite Micro direct (no WASM) ─────────────── */
    printk("--- B1: native_tflm ---\n");
    bench_b1_native_run();
    log_panic();
    k_msleep(200);

    /* ── B1 wasm_claw: WASM + capability gate (ai.infer granted) ────── */
    printk("--- B1: wasm_claw ---\n");
    bench_b1_wasm_run("wasm_claw");
    log_panic();
    k_msleep(200);

    /* ── B1 wasm_noguard: WASM, CG disabled (prj_noguard.conf) ─────── */
#ifdef CONFIG_BENCH_WASM_NOGUARD
    printk("--- B1: wasm_noguard ---\n");
    bench_b1_wasm_run("wasm_noguard");
    log_panic();
    k_msleep(200);
#endif

    /* ── B1 wasm_claw_mpu: ARM MPU only; on Xtensa emit mock directly ── */
    printk("--- B1: wasm_claw_mpu ---\n");
#ifdef CONFIG_SOC_SERIES_ESP32S3
    {
        bench_b1_result_t r = {
            .config      = "wasm_claw_mpu",
            .mock_mode   = true,
            .mock_reason = "ARM_MPU unavailable on ESP32-S3 (Xtensa LX7); "
                           "run on Cortex-M with prj_mpu.conf for real measurement",
        };
        bench_print_b1(&r);
    }
#else
    bench_b1_wasm_run("wasm_claw_mpu");
#endif
    log_panic();
    k_msleep(200);

    /* ── B2: sandbox_exec_begin / end domain-switch overhead ─────────── */
    printk("--- B2: MPU domain switch overhead ---\n");
    bench_b2_run();
    log_panic();
    k_msleep(200);

    /* ── B3: HMAC audit log write latency ───────────────────────────── */
    printk("--- B3: HMAC audit log ---\n");
    bench_b3_run();
    log_panic();
    k_msleep(200);

    /* ── B5: inference + BLE coexistence ───────────────────────────── */
    printk("--- B5: BLE coexistence ---\n");
    bench_b5_run();
    log_panic();
    k_msleep(200);

    printk("\n========================================\n");
    printk("  akiraclaw_bench COMPLETE\n");
    printk("========================================\n\n");

    return 0;
}
