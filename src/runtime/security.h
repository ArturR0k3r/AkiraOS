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
#define AKIRA_CAP_BLE        (1U << 5)  /* BLE app service: init, advertise, GATT */
#define AKIRA_CAP_STORAGE_READ (1U << 6)
#define AKIRA_CAP_STORAGE_WRITE (1U << 7)
#define AKIRA_CAP_NETWORK (1U << 8)
#define AKIRA_CAP_GPIO_READ (1U << 9)
#define AKIRA_CAP_GPIO_WRITE (1U << 10)
#define AKIRA_CAP_TIMER     (1U << 11)
#define AKIRA_CAP_UART      (1U << 12)
#define AKIRA_CAP_I2C       (1U << 13)
#define AKIRA_CAP_PWM       (1U << 14)
/* Elevated privilege — must not be granted to untrusted apps by default */
#define AKIRA_CAP_HID         (1U << 15)
#define AKIRA_CAP_APP_CONTROL (1U << 16)
#define AKIRA_CAP_IPC         (1U << 17)
/* Lightweight handoff: start another app and exit self — no stop/pause of
 * arbitrary apps.  Games and utilities can use this without full supervisor
 * power.  Manifest string: "app.switch" */
#define AKIRA_CAP_APP_SWITCH  (1U << 18)
/* Quota-enforced heap allocation from WASM-accessible memory.
 * Required to call mem_alloc / mem_free native APIs.
 * Manifest string: "memory" */
#define AKIRA_CAP_MEMORY      (1U << 19)
/* Read-only app identity: get own name, list apps, query status.
 * Does NOT grant start/stop authority — use app.control for that.
 * Manifest string: "app.info" */
#define AKIRA_CAP_APP_INFO    (1U << 20)
/* Read-only power & battery queries (mode, battery level/status).
 * Manifest string: "power.read" */
#define AKIRA_CAP_POWER_READ  (1U << 21)
/* Full power control: set sleep mode, configure wake sources.
 * Elevated privilege — do not grant to untrusted apps.
 * Manifest string: "power.control" */
#define AKIRA_CAP_POWER_CTRL  (1U << 22)
/* Persistent key-value settings read/write (NVS-backed).
 * Manifest string: "settings.*" */
#define AKIRA_CAP_SETTINGS    (1U << 23)

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