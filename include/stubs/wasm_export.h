/**
 * @file wasm_export.h
 * @brief Stub header used when CONFIG_AKIRA_WASM_RUNTIME is disabled.
 *
 * Provides empty type definitions so that AkiraOS API headers compile without
 * the real WAMR include tree.  This file must only be reachable when WAMR is
 * disabled — the CMakeLists.txt adds include/stubs/ to the include path only
 * in the else() branch of the CONFIG_AKIRA_WASM_RUNTIME guard.
 */

#ifndef AKIRA_WASM_EXPORT_STUB_H
#define AKIRA_WASM_EXPORT_STUB_H

#include <stdint.h>
#include <stdbool.h>

/* ── opaque handle stubs ─────────────────────────────────────────────────── */
typedef void *wasm_module_t;
typedef void *wasm_module_inst_t;
typedef void *wasm_function_inst_t;
typedef void *wasm_exec_env_t;
typedef void *wasm_memory_inst_t;
typedef void *wasm_shared_heap_t;

/* ── NativeSymbol (subset needed by akira_*_api.h registration macros) ───── */
#ifndef WASM_NATIVE_SYMBOL_DEFINED
#define WASM_NATIVE_SYMBOL_DEFINED
typedef struct NativeSymbol {
    const char *symbol;
    void       *func_ptr;
    const char *signature;
    void       *attachment;
} NativeSymbol;
#endif

/* ── Convenience macro stubs ─────────────────────────────────────────────── */
#ifndef WASM_EXPORT
#define WASM_EXPORT
#endif

#ifndef WASM_EXPORT_API_EXTERN
#define WASM_EXPORT_API_EXTERN
#endif

/* ── Value types (minimal) ───────────────────────────────────────────────── */
typedef uint8_t wasm_valkind_t;

typedef union wasm_val_internal_t {
    int32_t  i32;
    int64_t  i64;
    float    f32;
    double   f64;
} wasm_val_internal_t;

typedef struct wasm_val_t {
    wasm_valkind_t       kind;
    wasm_val_internal_t  of;
} wasm_val_t;

/* ── Runtime init stub (no-op when WAMR disabled) ────────────────────────── */
static inline bool wasm_runtime_init(void) { return false; }
static inline void wasm_runtime_destroy(void) {}

#endif /* AKIRA_WASM_EXPORT_STUB_H */
