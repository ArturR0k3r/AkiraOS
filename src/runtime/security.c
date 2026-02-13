#include "security.h"
#include <zephyr/logging/log.h>
#include <runtime/security/sandbox.h>

LOG_MODULE_REGISTER(akira_security, CONFIG_AKIRA_LOG_LEVEL);

#include <string.h>
#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

/* Map capability string to mask (exported) - extended for all capability types */
uint32_t akira_capability_str_to_mask(const char *cap)
{
    if (!cap) return 0;
    if (strcmp(cap, "display.write") == 0)  return AKIRA_CAP_DISPLAY_WRITE;
    if (strcmp(cap, "display.read") == 0)   return AKIRA_CAP_DISPLAY_WRITE;
    if (strcmp(cap, "input.read") == 0)     return AKIRA_CAP_INPUT_READ;
    if (strcmp(cap, "input.write") == 0)    return AKIRA_CAP_INPUT_WRITE;
    if (strcmp(cap, "sensor.read") == 0)    return AKIRA_CAP_SENSOR_READ;
    if (strcmp(cap, "rf.transceive") == 0)  return AKIRA_CAP_RF_TRANSCEIVE;
    if (strcmp(cap, "bt.shell") == 0)       return AKIRA_CAP_BT_SHELL;
    if (strcmp(cap, "storage.read") == 0)   return AKIRA_CAP_STORAGE_READ;
    if (strcmp(cap, "storage.write") == 0)  return AKIRA_CAP_STORAGE_WRITE;
    /* Wildcard patterns */
    if (strcmp(cap, "display.*") == 0)      return AKIRA_CAP_DISPLAY_WRITE;
    if (strcmp(cap, "input.*") == 0)        return AKIRA_CAP_INPUT_READ | AKIRA_CAP_INPUT_WRITE;
    if (strcmp(cap, "sensor.*") == 0)       return AKIRA_CAP_SENSOR_READ;
    if (strcmp(cap, "rf.*") == 0)           return AKIRA_CAP_RF_TRANSCEIVE;
    if (strcmp(cap, "bt.*") == 0)           return AKIRA_CAP_BT_SHELL;
    if (strcmp(cap, "storage.*") == 0)      return AKIRA_CAP_STORAGE_READ | AKIRA_CAP_STORAGE_WRITE;
    if (strcmp(cap, "network.*") == 0)      return AKIRA_CAP_NETWORK;
    if (strcmp(cap, "*") == 0)              return 0xFFFFFFFF;
    return 0;
}

char* akira_capability_mask_to_str(uint32_t cap)
{
    if (cap & AKIRA_CAP_DISPLAY_WRITE) return "display.write";
    if (cap & AKIRA_CAP_INPUT_READ) return "input.read";
    if (cap & AKIRA_CAP_INPUT_WRITE) return "input.write";
    if (cap & AKIRA_CAP_SENSOR_READ) return "sensor.read";
    if (cap & AKIRA_CAP_RF_TRANSCEIVE) return "rf.transceive";
    if (cap & AKIRA_CAP_BT_SHELL) return "bt.shell";
    if (cap & AKIRA_CAP_STORAGE_READ) return "storage.read";
    if (cap & AKIRA_CAP_STORAGE_WRITE) return "storage.write";
    return 0;
}

/* Convenience wrapper for native callers */
bool akira_security_check(uint32_t capability)
{
    return akira_security_check_native(capability);
}

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* Get capability mask for exec_env - for use with inline macros */
uint32_t akira_security_get_cap_mask(wasm_exec_env_t exec_env)
{
    if (!exec_env) return 0;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    return akira_runtime_get_cap_mask_for_module_inst(inst);
}

bool akira_security_check_exec(wasm_exec_env_t exec_env, uint32_t capability)
{
    if (!exec_env) return false;

    uint32_t mask = akira_security_get_cap_mask(exec_env);

    bool ok = (mask & capability) != 0;
    if (!ok) {
        wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
        char namebuf[32];
        if (akira_runtime_get_name_for_module_inst(inst, namebuf, sizeof(namebuf)) == 0) {
            LOG_WRN("Security: capability denied for app %s: %s", namebuf, akira_capability_mask_to_str(capability));
            sandbox_audit_log(AUDIT_EVENT_CAPABILITY_DENIED, namebuf, capability);
        } else {
            LOG_WRN("Security: capability denied for unknown app: %s", akira_capability_mask_to_str(capability));
            sandbox_audit_log(AUDIT_EVENT_CAPABILITY_DENIED, "unknown", capability);
        }
    }
    return ok;
}
#else
uint32_t akira_security_get_cap_mask(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return 0;
}

bool akira_security_check_exec(wasm_exec_env_t exec_env, uint32_t capability)
{
    (void)exec_env; (void)capability;
    return false;
}
#endif

bool akira_security_check_native(uint32_t capability)
{
    /* Native (non-wasm) callers have broader rights for now */
    (void)capability;
    return true;
}