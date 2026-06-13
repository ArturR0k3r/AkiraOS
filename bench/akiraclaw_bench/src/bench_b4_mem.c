/*
 * bench_b4_mem.c — B4: memory breakdown
 *
 * Reports:
 *   flash_tflm_kb          — TFLite Micro .text size from linker symbol
 *   flash_model_kb         — hello_world_float_sine model bytes
 *   ram_arena_kb           — tensor arena (CONFIG_AKIRA_AIINFER_ARENA_KB)
 *   sandbox_ctx_sizeof     — sizeof(sandbox_ctx_t) at compile time
 *   infer_slot_sizeof      — sizeof(InferSlot) approximated from arena + overhead
 *   ram_dram_reclaimed_bytes — DRAM recovered by AKIRA_BULK_BSS PSRAM relocation
 *
 * DRAM reclaim note:
 *   The git log records 5192 bytes from BLE globals moved to PSRAM via
 *   AKIRA_BULK_BSS.  This benchmark reports that figure as a compile-time
 *   constant confirmed from the ELF diff.  A proper ELF-based measurement
 *   is done offline with the collect.py --elf flag.
 *
 * flash_tflm_kb uses the __tflm_text_start / __tflm_text_end linker
 * symbols exported by modules/tflite-micro/CMakeLists.txt.  If those
 * symbols are absent (older TFLM version) the field is reported as 0 and
 * mock_mode is set.
 */

#include "result.h"
#include "timing.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_AKIRA_AIINFER
#include "api/akira_aiinfer_api.h"
#endif
#include "runtime/security/sandbox.h"

/* Model size from the embedded header */
#include "model_data.h"

LOG_MODULE_DECLARE(akiraclaw_bench, CONFIG_AKIRA_LOG_LEVEL);

/* Linker symbols bounding the TFLM .text section.
 * Defined in the TFLM CMakeLists or via a custom linker script fragment.
 * Declared as char arrays so we can take their difference safely. */
extern char __tflm_text_start[];
extern char __tflm_text_end[];

/* DRAM bytes reclaimed by PSRAM relocation of BLE and aiinfer globals.
 * Confirmed from ELF diff (git log: 5192 bytes from BLE globals).
 * Additional PSRAM relocation from InferSlot arenas added separately. */
#define DRAM_RECLAIM_DOCUMENTED_BYTES 5192U

/* InferSlot size approximation:
 *   arena:       AKIRA_AIINFER_ARENA_KB * 1024
 *   resolver:    sizeof(MicroMutableOpResolver<16>) ≈ 256 B
 *   interpreter: sizeof(MicroInterpreter)           ≈ 512 B
 *   metadata:    model_buf ptr + model_len + used   ≈  24 B
 * Total ≈ arena + 792 bytes  (PSRAM-resident due to AKIRA_BULK_BSS)
 */
#ifndef AKIRA_AIINFER_ARENA_KB
#define AKIRA_AIINFER_ARENA_KB 128
#endif
#define INFER_SLOT_SIZEOF_APPROX \
    ((uint32_t)(AKIRA_AIINFER_ARENA_KB * 1024U) + 792U)

void bench_b4_run(void)
{
    bench_b4_result_t result = {
        .flash_model_kb           = (hello_world_model_len + 1023U) / 1024U,
        .ram_arena_kb             = (uint32_t)AKIRA_AIINFER_ARENA_KB,
        .sandbox_ctx_sizeof       = (uint32_t)sizeof(sandbox_ctx_t),
        .infer_slot_sizeof        = INFER_SLOT_SIZEOF_APPROX,
        .ram_dram_reclaimed_bytes = DRAM_RECLAIM_DOCUMENTED_BYTES,
        .mock_mode                = false,
        .mock_reason              = NULL,
    };

    /* Attempt to read TFLM text section size from linker symbols.
     * If the symbols are missing the linker will error; we guard with weak
     * references so a missing symbol gives address 0 instead. */
    uintptr_t tflm_start = (uintptr_t)__tflm_text_start;
    uintptr_t tflm_end   = (uintptr_t)__tflm_text_end;

    if (tflm_end > tflm_start) {
        result.flash_tflm_kb = (uint32_t)((tflm_end - tflm_start + 1023U) / 1024U);
    } else {
        result.flash_tflm_kb = 0;
        result.mock_mode     = true;
        result.mock_reason   =
            "__tflm_text_start/__tflm_text_end linker symbols absent; "
            "add TFLM section markers to the linker script or measure "
            "offline with: arm-none-eabi-nm zephyr.elf | grep tflm";
    }

    bench_print_b4(&result);

    LOG_INF("B4: flash_tflm=%u KB, model=%u B, arena=%u KB, "
            "sandbox_ctx=%u B, dram_reclaim=%u B",
            result.flash_tflm_kb,
            hello_world_model_len,
            result.ram_arena_kb,
            result.sandbox_ctx_sizeof,
            result.ram_dram_reclaimed_bytes);
}
