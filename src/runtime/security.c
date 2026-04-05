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
    if (strcmp(cap, "ble") == 0)            return AKIRA_CAP_BLE;
    if (strcmp(cap, "bt.shell") == 0)       return AKIRA_CAP_BLE; /* legacy alias */
    if (strcmp(cap, "storage.read") == 0)   return AKIRA_CAP_STORAGE_READ;
    if (strcmp(cap, "storage.write") == 0)  return AKIRA_CAP_STORAGE_WRITE;
    if (strcmp(cap, "gpio.read") == 0)      return AKIRA_CAP_GPIO_READ;
    if (strcmp(cap, "gpio.write") == 0)     return AKIRA_CAP_GPIO_WRITE;
    if (strcmp(cap, "timer") == 0)          return AKIRA_CAP_TIMER;
    if (strcmp(cap, "uart") == 0)           return AKIRA_CAP_UART;
    if (strcmp(cap, "i2c") == 0)            return AKIRA_CAP_I2C;
    if (strcmp(cap, "pwm") == 0)            return AKIRA_CAP_PWM;
    if (strcmp(cap, "hid") == 0)            return AKIRA_CAP_HID;
    if (strcmp(cap, "app.control") == 0)    return AKIRA_CAP_APP_CONTROL;
    if (strcmp(cap, "ipc") == 0)            return AKIRA_CAP_IPC;
    if (strcmp(cap, "app.switch") == 0)     return AKIRA_CAP_APP_SWITCH;
    if (strcmp(cap, "memory") == 0)         return AKIRA_CAP_MEMORY;
    if (strcmp(cap, "memory.alloc") == 0)   return AKIRA_CAP_MEMORY;
    if (strcmp(cap, "app.info") == 0)       return AKIRA_CAP_APP_INFO;
    if (strcmp(cap, "power.read") == 0)     return AKIRA_CAP_POWER_READ;
    if (strcmp(cap, "power.control") == 0)  return AKIRA_CAP_POWER_CTRL;
    if (strcmp(cap, "power.*") == 0)        return AKIRA_CAP_POWER_READ | AKIRA_CAP_POWER_CTRL;
    if (strcmp(cap, "settings.read") == 0)  return AKIRA_CAP_SETTINGS;
    if (strcmp(cap, "settings.write") == 0) return AKIRA_CAP_SETTINGS;
    if (strcmp(cap, "settings.*") == 0)     return AKIRA_CAP_SETTINGS;
    /* Wildcard patterns */
    if (strcmp(cap, "display.*") == 0)      return AKIRA_CAP_DISPLAY_WRITE;
    if (strcmp(cap, "input.*") == 0)        return AKIRA_CAP_INPUT_READ | AKIRA_CAP_INPUT_WRITE;
    if (strcmp(cap, "sensor.*") == 0)       return AKIRA_CAP_SENSOR_READ;
    if (strcmp(cap, "rf.*") == 0)           return AKIRA_CAP_RF_TRANSCEIVE;
    if (strcmp(cap, "bt.*") == 0)           return AKIRA_CAP_BLE | AKIRA_CAP_HID;
    if (strcmp(cap, "storage.*") == 0)      return AKIRA_CAP_STORAGE_READ | AKIRA_CAP_STORAGE_WRITE;
    if (strcmp(cap, "gpio.*") == 0)         return AKIRA_CAP_GPIO_READ | AKIRA_CAP_GPIO_WRITE;
    if (strcmp(cap, "network.*") == 0)      return AKIRA_CAP_NETWORK;
    if (strcmp(cap, "hw.*") == 0)           return AKIRA_CAP_TIMER | AKIRA_CAP_UART | AKIRA_CAP_I2C | AKIRA_CAP_PWM;
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
    if (cap & AKIRA_CAP_BLE) return "ble";
    if (cap & AKIRA_CAP_STORAGE_READ) return "storage.read";
    if (cap & AKIRA_CAP_STORAGE_WRITE) return "storage.write";
    if (cap & AKIRA_CAP_GPIO_READ) return "gpio.read";
    if (cap & AKIRA_CAP_GPIO_WRITE) return "gpio.write";
    if (cap & AKIRA_CAP_TIMER) return "timer";
    if (cap & AKIRA_CAP_UART)  return "uart";
    if (cap & AKIRA_CAP_I2C)   return "i2c";
    if (cap & AKIRA_CAP_PWM)   return "pwm";
    if (cap & AKIRA_CAP_HID)         return "hid";
    if (cap & AKIRA_CAP_APP_CONTROL) return "app.control";
    if (cap & AKIRA_CAP_IPC)         return "ipc";
    if (cap & AKIRA_CAP_APP_SWITCH)  return "app.switch";
    if (cap & AKIRA_CAP_APP_INFO)    return "app.info";
    if (cap & AKIRA_CAP_MEMORY)      return "memory";
    if (cap & AKIRA_CAP_POWER_READ)  return "power.read";
    if (cap & AKIRA_CAP_POWER_CTRL)  return "power.control";
    return "unknown";
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