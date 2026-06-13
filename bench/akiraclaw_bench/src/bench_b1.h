/* bench_b1.h — shared declarations for B1 sub-modules */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* native_tflm: TFLite Micro called directly from C++ (no WASM) */
void bench_b1_native_run(void);

/* wasm_noguard / wasm_claw / wasm_claw_mpu:
 * Load the akiraclaw_bench WASM module via the AkiraOS runtime and
 * collect the JSON results it prints to the console.
 * @param config  one of "wasm_noguard", "wasm_claw", "wasm_claw_mpu"
 */
void bench_b1_wasm_run(const char *config);

#ifdef __cplusplus
}
#endif
