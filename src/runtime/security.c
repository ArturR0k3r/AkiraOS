#include "security.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_security, CONFIG_AKIRA_LOG_LEVEL);

#include <string.h>
#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

/* Map capability string to mask (exported) */
uint32_t akira_capability_str_to_mask(const char *cap)
{
    if (!cap) return 0;
    if (strcmp(cap, "display.write") == 0) return AKIRA_CAP_DISPLAY_WRITE;
    if (strcmp(cap, "input.read") == 0) return AKIRA_CAP_INPUT_READ;
    if (strcmp(cap, "sensor.read") == 0) return AKIRA_CAP_SENSOR_READ;
    if (strcmp(cap, "rf.transceive") == 0) return AKIRA_CAP_RF_TRANSCEIVE;
    return 0;
}

/* Convenience wrapper for native callers */
bool akira_security_check(const char *capability)
{
    return akira_security_check_native(capability);
}

#ifdef CONFIG_AKIRA_WASM_RUNTIME
bool akira_security_check_exec(wasm_exec_env_t exec_env, const char *capability)
{
    if (!exec_env || !capability) return false;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    uint32_t mask = akira_runtime_get_cap_mask_for_module_inst(inst);

    uint32_t req = akira_capability_str_to_mask(capability);
    if (req == 0) {
        LOG_WRN("Security: unknown capability requested: %s", capability);
        return false;
    }

    bool ok = (mask & req) != 0;
    if (!ok) {
        char namebuf[32];
        if (akira_runtime_get_name_for_module_inst(inst, namebuf, sizeof(namebuf)) == 0)
            LOG_WRN("Security: capability denied for app %s: %s", namebuf, capability);
        else
            LOG_WRN("Security: capability denied for unknown app: %s", capability);
    }
    return ok;
}
#else
bool akira_security_check_exec(wasm_exec_env_t exec_env, const char *capability)
{
    (void)exec_env; (void)capability;
    return false;
}
#endif

bool akira_security_check_native(const char *capability)
{
    /* Native (non-wasm) callers have broader rights for now */
    (void)capability;
    return true;
}