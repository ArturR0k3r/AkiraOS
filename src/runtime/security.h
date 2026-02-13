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


#define AKIRA_CAP_DISPLAY_WRITE (1U << 0)
#define AKIRA_CAP_INPUT_READ (1U << 1)
#define AKIRA_CAP_INPUT_WRITE (1U << 2)
#define AKIRA_CAP_SENSOR_READ (1U << 3)
#define AKIRA_CAP_RF_TRANSCEIVE (1U << 4)
#define AKIRA_CAP_BT_SHELL (1U << 5)
#define AKIRA_CAP_STORAGE_READ (1U << 6)
#define AKIRA_CAP_STORAGE_WRITE (1U << 7)
#define AKIRA_CAP_NETWORK (1U << 8)

/*
 * Capability check macro using security subsystem.
 * Delegates to akira_security_check_exec() for centralized permission validation.
 * For WASM API use with string-based capability names.
 *
 * @param exec_env     The WASM execution environment
 * @param capability   The capability name to check (e.g., AKIRA_CAP_DISPLAY_WRITE)
 * @return             true if capability is granted, false otherwise
 *
 * Note: Involves function call overhead. For performance-critical paths,
 * consider caching capability check results when possible.
 */
#define AKIRA_CHECK_CAP_INLINE(exec_env, capability) \
    akira_security_check_exec(exec_env, capability)

/*
 * Inline capability check with early return for functions returning int.
 * Usage: AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_<CAPABILITY>, -EPERM);
 */
#define AKIRA_CHECK_CAP_OR_RETURN(exec_env, capability, retval) \
    do { \
        if (!akira_security_check_exec(exec_env, capability)) { \
            return (retval); \
        } \
    } while (0)

/*
 * Inline capability check with early return for void functions.
 */
#define AKIRA_CHECK_CAP_OR_RETURN_VOID(exec_env, capability) \
    do { \
        if (!akira_security_check_exec(exec_env, capability)) { \
            return; \
        } \
    } while (0)

/* Central capability guard used by native APIs and runtime */
bool akira_security_check_exec(wasm_exec_env_t exec_env, uint32_t capability);
bool akira_security_check_native(uint32_t capability);

/* Convenience wrapper used by native API implementations (non-wasm callers) */
bool akira_security_check(uint32_t capability);

/* Get the current app's capability mask from exec_env - for use with inline macros */
uint32_t akira_security_get_cap_mask(wasm_exec_env_t exec_env);

/* Capability string to mask helper (public so runtime can parse manifests).
 * This maps capability strings like "display.write" -> AKIRA_CAP_DISPLAY_WRITE
 */
uint32_t akira_capability_str_to_mask(const char *cap);
/* Mask to string helper for logging (returns first matching capability string) */
char* akira_capability_mask_to_str(uint32_t cap);

/* Runtime helpers used by security implementation */
#include <stddef.h>
#include <stdint.h>
uint32_t akira_runtime_get_cap_mask_for_module_inst(wasm_module_inst_t inst);
int akira_runtime_get_name_for_module_inst(wasm_module_inst_t inst, char *buf, size_t buflen);


#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RUNTIME_SECURITY_H */