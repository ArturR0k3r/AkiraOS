/*
 * result.h — JSON result printer for akiraclaw_bench
 *
 * Outputs one JSON object per line to the Zephyr console (printk).
 * The host-side collect.py script parses these out of the serial log.
 *
 * mock_mode=true is set whenever a measurement path is unavailable on
 * the current platform (e.g. ARM MPU on Xtensa).  The "reason" field
 * explains why.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/sys/printk.h>

/* ── Sentinel printed before/after each JSON object for reliable parsing ── */
#define BENCH_JSON_START "<<BENCH_JSON_START>>"
#define BENCH_JSON_END   "<<BENCH_JSON_END>>"

/* ── B1 Inference latency result ─────────────────────────────────────────── */
typedef struct {
    const char *config;       /* "native_tflm" | "wasm_noguard" | "wasm_claw" | "wasm_claw_mpu" */
    uint32_t    load_us;
    uint32_t    run_avg_us;
    uint32_t    run_p99_us;
    uint32_t    run_max_us;
    uint32_t    unload_us;
    uint32_t    iterations;
    uint32_t    model_size_bytes;
    uint32_t    loop_overhead_us;
    bool        ble_active;
    bool        mock_mode;
    const char *mock_reason;
} bench_b1_result_t;

static inline void bench_print_b1(const bench_b1_result_t *r)
{
    printk("%s\n", BENCH_JSON_START);
    printk("{\n");
    printk("  \"bench\": \"B1\",\n");
    printk("  \"config\": \"%s\",\n", r->config);
    printk("  \"platform\": \"esp32s3_akiraconsole\",\n");
    printk("  \"load_us\": %u,\n", r->load_us);
    printk("  \"run_avg_us\": %u,\n", r->run_avg_us);
    printk("  \"run_p99_us\": %u,\n", r->run_p99_us);
    printk("  \"run_max_us\": %u,\n", r->run_max_us);
    printk("  \"unload_us\": %u,\n", r->unload_us);
    printk("  \"iterations\": %u,\n", r->iterations);
    printk("  \"model\": \"hello_world_float_sine\",\n");
    printk("  \"model_size_bytes\": %u,\n", r->model_size_bytes);
    printk("  \"loop_overhead_us\": %u,\n", r->loop_overhead_us);
    printk("  \"ble_active\": %s,\n", r->ble_active ? "true" : "false");
    printk("  \"mock_mode\": %s,\n", r->mock_mode ? "true" : "false");
    if (r->mock_mode && r->mock_reason) {
        printk("  \"mock_reason\": \"%s\"\n", r->mock_reason);
    } else {
        printk("  \"mock_reason\": null\n");
    }
    printk("}\n");
    printk("%s\n", BENCH_JSON_END);
}

/* ── B2 MPU domain switch overhead ──────────────────────────────────────── */
typedef struct {
    uint32_t    begin_avg_ns;
    uint32_t    begin_p99_ns;
    uint32_t    begin_max_ns;
    uint32_t    end_avg_ns;
    uint32_t    end_p99_ns;
    uint32_t    end_max_ns;
    uint32_t    iterations;
    bool        mock_mode;
    const char *mock_reason;
} bench_b2_result_t;

static inline void bench_print_b2(const bench_b2_result_t *r)
{
    printk("%s\n", BENCH_JSON_START);
    printk("{\n");
    printk("  \"bench\": \"B2\",\n");
    printk("  \"platform\": \"esp32s3_akiraconsole\",\n");
    printk("  \"sandbox_exec_begin_avg_ns\": %u,\n", r->begin_avg_ns);
    printk("  \"sandbox_exec_begin_p99_ns\": %u,\n", r->begin_p99_ns);
    printk("  \"sandbox_exec_begin_max_ns\": %u,\n", r->begin_max_ns);
    printk("  \"sandbox_exec_end_avg_ns\": %u,\n", r->end_avg_ns);
    printk("  \"sandbox_exec_end_p99_ns\": %u,\n", r->end_p99_ns);
    printk("  \"sandbox_exec_end_max_ns\": %u,\n", r->end_max_ns);
    printk("  \"iterations\": %u,\n", r->iterations);
    printk("  \"mock_mode\": %s,\n", r->mock_mode ? "true" : "false");
    if (r->mock_mode && r->mock_reason) {
        printk("  \"mock_reason\": \"%s\"\n", r->mock_reason);
    } else {
        printk("  \"mock_reason\": null\n");
    }
    printk("}\n");
    printk("%s\n", BENCH_JSON_END);
}

/* ── B3 HMAC audit log overhead ──────────────────────────────────────────── */
typedef struct {
    uint32_t    audit_write_avg_ns;
    uint32_t    audit_write_p99_ns;
    uint32_t    audit_write_max_ns;
    uint32_t    audit_nohmac_avg_ns; /* baseline without HMAC */
    uint32_t    iterations;
    bool        hmac_enabled;
    bool        mock_mode;
    const char *mock_reason;
} bench_b3_result_t;

static inline void bench_print_b3(const bench_b3_result_t *r)
{
    printk("%s\n", BENCH_JSON_START);
    printk("{\n");
    printk("  \"bench\": \"B3\",\n");
    printk("  \"platform\": \"esp32s3_akiraconsole\",\n");
    printk("  \"audit_write_avg_ns\": %u,\n", r->audit_write_avg_ns);
    printk("  \"audit_write_p99_ns\": %u,\n", r->audit_write_p99_ns);
    printk("  \"audit_write_max_ns\": %u,\n", r->audit_write_max_ns);
    printk("  \"audit_nohmac_avg_ns\": %u,\n", r->audit_nohmac_avg_ns);
    printk("  \"hmac_enabled\": %s,\n", r->hmac_enabled ? "true" : "false");
    printk("  \"iterations\": %u,\n", r->iterations);
    printk("  \"mock_mode\": %s,\n", r->mock_mode ? "true" : "false");
    if (r->mock_mode && r->mock_reason) {
        printk("  \"mock_reason\": \"%s\"\n", r->mock_reason);
    } else {
        printk("  \"mock_reason\": null\n");
    }
    printk("}\n");
    printk("%s\n", BENCH_JSON_END);
}

/* ── B4 Memory breakdown ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t flash_tflm_kb;
    uint32_t flash_model_kb;
    uint32_t ram_arena_kb;
    uint32_t sandbox_ctx_sizeof;
    uint32_t infer_slot_sizeof;
    uint32_t ram_dram_reclaimed_bytes;
    bool     mock_mode;
    const char *mock_reason;
} bench_b4_result_t;

static inline void bench_print_b4(const bench_b4_result_t *r)
{
    printk("%s\n", BENCH_JSON_START);
    printk("{\n");
    printk("  \"bench\": \"B4\",\n");
    printk("  \"platform\": \"esp32s3_akiraconsole\",\n");
    printk("  \"flash_tflm_kb\": %u,\n", r->flash_tflm_kb);
    printk("  \"flash_model_kb\": %u,\n", r->flash_model_kb);
    printk("  \"ram_arena_kb\": %u,\n", r->ram_arena_kb);
    printk("  \"sandbox_ctx_sizeof\": %u,\n", r->sandbox_ctx_sizeof);
    printk("  \"infer_slot_sizeof_approx\": %u,\n", r->infer_slot_sizeof);
    printk("  \"ram_dram_reclaimed_bytes\": %u,\n", r->ram_dram_reclaimed_bytes);
    printk("  \"mock_mode\": %s,\n", r->mock_mode ? "true" : "false");
    if (r->mock_mode && r->mock_reason) {
        printk("  \"mock_reason\": \"%s\"\n", r->mock_reason);
    } else {
        printk("  \"mock_reason\": null\n");
    }
    printk("}\n");
    printk("%s\n", BENCH_JSON_END);
}

/* ── B5 BLE coexistence ──────────────────────────────────────────────────── */
typedef struct {
    uint32_t    infer_avg_us;
    uint32_t    infer_p99_us;
    uint32_t    infer_max_us;
    uint32_t    infer_avg_us_ble_off; /* same measurement with BLE off */
    uint32_t    missed_adv_events;
    uint32_t    total_adv_windows;
    uint32_t    iterations;
    bool        ble_active;
    bool        mock_mode;
    const char *mock_reason;
} bench_b5_result_t;

static inline void bench_print_b5(const bench_b5_result_t *r)
{
    printk("%s\n", BENCH_JSON_START);
    printk("{\n");
    printk("  \"bench\": \"B5\",\n");
    printk("  \"platform\": \"esp32s3_akiraconsole\",\n");
    printk("  \"infer_avg_us\": %u,\n", r->infer_avg_us);
    printk("  \"infer_p99_us\": %u,\n", r->infer_p99_us);
    printk("  \"infer_max_us\": %u,\n", r->infer_max_us);
    printk("  \"infer_avg_us_ble_off\": %u,\n", r->infer_avg_us_ble_off);
    printk("  \"missed_adv_events\": %u,\n", r->missed_adv_events);
    printk("  \"total_adv_windows\": %u,\n", r->total_adv_windows);
    printk("  \"iterations\": %u,\n", r->iterations);
    printk("  \"ble_active\": %s,\n", r->ble_active ? "true" : "false");
    printk("  \"mock_mode\": %s,\n", r->mock_mode ? "true" : "false");
    if (r->mock_mode && r->mock_reason) {
        printk("  \"mock_reason\": \"%s\"\n", r->mock_reason);
    } else {
        printk("  \"mock_reason\": null\n");
    }
    printk("}\n");
    printk("%s\n", BENCH_JSON_END);
}
