/**
 * @file wamr_config.h
 * @brief WAMR Runtime Configuration and Initialization
 *
 * Defines WAMR initialization parameters, memory management,
 * and native API registration for AkiraOS.
 */

#ifndef WAMR_CONFIG_H
#define WAMR_CONFIG_H

#ifdef CONFIG_WAMR_ENABLE

#include "wasm_export.h"
#include <stdint.h>

/* ===== Memory Configuration ===== */

/** Total heap size for WAMR (in bytes) */
#ifdef CONFIG_WAMR_HEAP_SIZE
#define WAMR_HEAP_SIZE CONFIG_WAMR_HEAP_SIZE
#else
#define WAMR_HEAP_SIZE (512 * 1024)  /* 512 KB default */
#endif

/** Default stack size for WASM instances */
#define WAMR_STACK_SIZE (256 * 1024)  /* 256 KB per instance */

/** Default heap size per WASM instance */
#define WAMR_INSTANCE_HEAP (64 * 1024)  /* 64 KB per instance */

/* ===== Thread Configuration ===== */

/** Maximum number of concurrent WASM threads */
#define WAMR_MAX_THREAD_NUM 4

/* ===== Instance Management ===== */

/** Maximum WASM instances allowed */
#define MAX_WASM_INSTANCES 8

/** Maximum path length for WASM app names */
#define MAX_APP_PATH_LEN 64

/** Maximum WASM app name length */
#define MAX_APP_NAME_LEN 32


/* ===== Execution Environment ===== */

/**
 * @brief Execution context for WASM function calls
 *
 * Manages execution environment, call stack, and result storage.
 */
typedef struct {
    wasm_exec_env_t exec_env;           /**< WAMR execution environment */
    wasm_module_inst_t module_inst;     /**< WASM module instance */
    char name[MAX_APP_NAME_LEN];        /**< Application name */
    uint32_t heap_size;                 /**< Instance heap size in bytes */
    uint64_t call_count;                /**< Total function calls made */
    int32_t last_error;                 /**< Last error code */
} wamr_exec_context_t;

/* ===== Native API Configuration ===== */

/**
 * @brief Native function signature patterns
 *
 * Used for validating and marshaling parameters between WASM and native code.
 *
 * Signature characters:
 *   'i' = i32 (4 bytes)
 *   'I' = i64 (8 bytes)
 *   'f' = f32 (4 bytes)
 *   'F' = f64 (8 bytes)
 *   '*' = buffer pointer (WASM address space)
 *   '~' = buffer length (follows after '*')
 *   '$' = string pointer (null-terminated)
 *   'r' = externref / GC reference
 *
 * Examples:
 *   "(ii)i"     = two i32 params, returns i32
 *   "(*~)i"     = buffer + length params, returns i32
 *   "($)i"      = string param, returns i32
 */

/* ===== Callback Types ===== */

/**
 * @brief Callback for native function execution
 *
 * @param exec_env Execution environment
 * @param... Variable arguments per function signature
 * @return Result value (typically i32 or i64)
 */
typedef int32_t (*wamr_native_func_t)(wasm_exec_env_t exec_env, ...);

/**
 * @brief Callback for WASM instance lifecycle events
 *
 * @param instance_id ID of the WASM instance
 * @param event Type of lifecycle event
 * @return 0 on success, negative on error
 */
typedef int (*wamr_lifecycle_callback_t)(int instance_id, uint32_t event);

/* Lifecycle event types */
#define WAMR_EVENT_CREATED  0x01
#define WAMR_EVENT_STARTED  0x02
#define WAMR_EVENT_STOPPED  0x03
#define WAMR_EVENT_DESTROYED 0x04
#define WAMR_EVENT_ERROR    0x05

/* ===== Utility Macros ===== */

/**
 * @brief Export a native function with signature
 *
 * Creates a NativeSymbol entry for registering with WAMR.
 *
 * @param func_name The function name (must match WASM symbol)
 * @param signature The function signature string
 *
 * Example:
 *   EXPORT_NATIVE_FUNC(my_function, "(ii)i")
 */
#define EXPORT_NATIVE_FUNC(func_name, signature) \
    { \
        #func_name, \
        (void *)func_name, \
        signature \
    }

/**
 * @brief Get WASM module instance from execution environment
 *
 * @param exec_env Execution environment
 * @return Module instance pointer
 */
#define WAMR_GET_MODULE_INST(exec_env) \
    wasm_runtime_get_module_inst(exec_env)

/* ===== Initialization Functions ===== */

/**
 * @brief Initialize WAMR runtime with AkiraOS configuration
 *
 * Sets up memory management, native APIs, threading support, and
 * other AkiraOS-specific WAMR parameters.
 *
 * @return 0 on success, negative error code on failure
 */
int wamr_init_runtime(void);

/**
 * @brief Cleanup and destroy WAMR runtime
 *
 * Releases all allocated resources and terminates WAMR.
 *
 * @return 0 on success, negative error code on failure
 */
int wamr_destroy_runtime(void);

/**
 * @brief Register AkiraOS native functions with WAMR
 *
 * Exports all native APIs that WASM applications can call.
 *
 * @return 0 on success, negative error code on failure
 */
int wamr_register_native_apis(void);

/**
 * @brief Get current WAMR engine instance
 *
 * @return Pointer to wasm_engine_t or NULL if not initialized
 */
wasm_engine_t wamr_get_engine(void);

/**
 * @brief Check if WAMR is initialized
 *
 * @return true if initialized, false otherwise
 */
bool wamr_is_initialized(void);

/* ===== Error Handling ===== */

/**
 * @brief Convert WAMR error to errno
 *
 * @param error_buf WAMR error buffer
 * @return Negative errno value
 */
int wamr_error_to_errno(const char *error_buf);

/**
 * @brief Get human-readable WAMR error message
 *
 * @param exec_env Execution environment (or NULL)
 * @return Error message string
 */
const char *wamr_get_error_message(wasm_exec_env_t exec_env);

#endif /* CONFIG_WAMR_ENABLE */

#endif /* WAMR_CONFIG_H */
