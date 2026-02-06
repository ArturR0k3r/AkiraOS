#include "akira_api.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_log_api, CONFIG_LOG_DEFAULT_LEVEL);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
int akira_native_log(wasm_exec_env_t exec_env, uint32_t level, char* message){
    
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) return -1;

    switch (level)
    {
    case LOG_LEVEL_ERR:
        LOG_ERR("Logged from wasm app %s",message);
        break;
    case LOG_LEVEL_WRN:
        LOG_WRN("Logged from wasm app %s",message);
        break;
    case LOG_LEVEL_INF:
        LOG_INF("Logged from wasm app %s",message);
        break;
    case LOG_LEVEL_DBG:
        LOG_DBG("Logged from wasm app %s",message);
        break;
    default:
        LOG_INF("UNKOWN TYPE pushed from wasm app (%d)", level);
        break;
    }

    return 0;
}
#endif
