#ifndef AKIRA_RUNTIME_SECURITY_H
#define AKIRA_RUNTIME_SECURITY_H

#include <stdbool.h>
#include <stdint.h>

/* WAMR headers are optional; provide lightweight typedefs when WAMR is not enabled */
#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#else
/* Provide opaque types so code can compile when WAMR is disabled (stubs) */
typedef void *wasm_exec_env_t;
typedef void *wasm_module_inst_t;
typedef void *wasm_module_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Capability bits (shared) - use powers of 2 for efficient masking */
#define AKIRA_CAP_DISPLAY_WRITE   (1U << 0)
#define AKIRA_CAP_INPUT_READ      (1U << 1)
#define AKIRA_CAP_INPUT_WRITE     (1U << 2)
#define AKIRA_CAP_SENSOR_READ     (1U << 3)
#define AKIRA_CAP_RF_TRANSCEIVE   (1U << 4)
#define AKIRA_CAP_BT_SHELL       (1U << 5)
/*
 * Performance-critical inline capability check macro.
 * Checks if a capability mask bit is set - avoids function call overhead.
 * For native API use with pre-computed mask values.
 *
 * @param cap_mask  The app's capability bitmask (uint32_t)
 * @param cap_bit   The capability bit to check (e.g., AKIRA_CAP_DISPLAY_WRITE)
 * @return          true if capability is granted, false otherwise
 *
 * Target: <60ns overhead per check (single bitwise AND + branch)
 */
#define AKIRA_CHECK_CAP_INLINE(cap_mask, cap_bit) \
    (((cap_mask) & (cap_bit)) != 0)

/*
 * Inline capability check with early return for functions returning int.
 * Usage: AKIRA_CHECK_CAP_OR_RETURN(app_cap_mask, AKIRA_CAP_RF_TRANSCEIVE, -EPERM);
 */
#define AKIRA_CHECK_CAP_OR_RETURN(cap_mask, cap_bit, retval) \
    do { \
        if (!AKIRA_CHECK_CAP_INLINE(cap_mask, cap_bit)) { \
            return (retval); \
        } \
    } while (0)

/*
 * Inline capability check with early return for void functions.
 */
#define AKIRA_CHECK_CAP_OR_RETURN_VOID(cap_mask, cap_bit) \
    do { \
        if (!AKIRA_CHECK_CAP_INLINE(cap_mask, cap_bit)) { \
            return; \
        } \
    } while (0)

/* Central capability guard used by native APIs and runtime */
bool akira_security_check_exec(wasm_exec_env_t exec_env, const char *capability);
bool akira_security_check_native(const char *capability);

/* Convenience wrapper used by native API implementations (non-wasm callers) */
bool akira_security_check(const char *capability);

/* Get the current app's capability mask from exec_env - for use with inline macros */
uint32_t akira_security_get_cap_mask(wasm_exec_env_t exec_env);

/* Capability string to mask helper (public so runtime can parse manifests).
 * This maps capability strings like "display.write" -> AKIRA_CAP_DISPLAY_WRITE
 */
uint32_t akira_capability_str_to_mask(const char *capability);

/* Runtime helpers used by security implementation */
#include <stddef.h>
#include <stdint.h>
uint32_t akira_runtime_get_cap_mask_for_module_inst(wasm_module_inst_t inst);
int akira_runtime_get_name_for_module_inst(wasm_module_inst_t inst, char *buf, size_t buflen);


#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RUNTIME_SECURITY_H */