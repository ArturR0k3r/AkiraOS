#include "akira_api.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_log_api, CONFIG_LOG_DEFAULT_LEVEL);

void akira_log(int level, const char *message){
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
}