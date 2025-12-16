/**
 * @file system_manager.c
 * @brief Main System Manager Implementation
 */

#include "system_manager.h"
#include "event_bus.h"
#include "init_table.h"
#include "hardware_manager.h"
#include "network_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#ifdef CONFIG_AKIRA_SETTINGS
#include "../settings/settings.h"
#endif

#ifdef CONFIG_AKIRA_STORAGE_FATFS
#include "../storage/fatfs/fatfs_manager.h"
#endif

#ifdef CONFIG_AKIRA_OTA_MANAGER
#include "../OTA/ota_manager.h"
#endif

#ifdef CONFIG_AKIRA_APP_MANAGER
#include "../apps/app_manager.h"
#endif

#ifdef CONFIG_AKIRA_OCRE_RUNTIME
#include "../runtime/ocre/ocre_runtime.h"
#endif

#ifdef CONFIG_AKIRA_WASM_MANAGER
#include "../services/wasm/wasm_app_manager.h"
#endif

#ifdef CONFIG_AKIRA_SHELL
#include "../shell/akira_shell.h"
#endif

#ifdef CONFIG_AKIRA_HTTP_SERVER
#include "../connectivity/http/http_server.h"
#endif

LOG_MODULE_REGISTER(sys_manager, CONFIG_AKIRA_LOG_LEVEL);

static struct {
    bool initialized;
    bool ready;
    uint64_t boot_time;
} sys_state = {0};

/* Forward declarations of subsystem init functions */
static int init_storage_subsystem(void);
static int init_settings_subsystem(void);
static int init_app_subsystem(void);
static int init_services_subsystem(void);
static int init_shell_subsystem(void);

static int init_storage_subsystem(void)
{
#ifdef CONFIG_AKIRA_STORAGE_FATFS
    int ret = fatfs_manager_init();
    if (ret < 0) {
        LOG_ERR("Storage initialization failed: %d", ret);
        return ret;
    }
    
    system_event_t event = {
        .type = EVENT_STORAGE_READY,
        .timestamp = k_uptime_get()
    };
    event_bus_publish(&event);
    
    return 0;
#else
    LOG_DBG("Storage not configured");
    return 0;
#endif
}

static int init_settings_subsystem(void)
{
#ifdef CONFIG_AKIRA_SETTINGS
    int ret = settings_init();
    if (ret < 0) {
        LOG_ERR("Settings initialization failed: %d", ret);
        return ret;
    }
    return 0;
#else
    LOG_DBG("Settings not configured");
    return 0;
#endif
}

static int init_app_subsystem(void)
{
    int ret;
    
#ifdef CONFIG_AKIRA_OCRE_RUNTIME
    ret = ocre_runtime_init();
    if (ret < 0) {
        LOG_ERR("OCRE runtime initialization failed: %d", ret);
        return ret;
    }
#endif

#ifdef CONFIG_AKIRA_APP_MANAGER
    ret = app_manager_init();
    if (ret < 0) {
        LOG_ERR("App manager initialization failed: %d", ret);
        return ret;
    }
#endif

#ifdef CONFIG_AKIRA_WASM_MANAGER
    ret = wasm_app_manager_init();
    if (ret < 0) {
        LOG_ERR("WASM app manager initialization failed: %d", ret);
        return ret;
    }
#endif

#ifdef CONFIG_AKIRA_OTA_MANAGER
    ret = ota_manager_init();
    if (ret < 0) {
        LOG_WRN("OTA manager initialization failed: %d", ret);
        /* Non-critical */
    }
#endif

    return 0;
}

static int init_services_subsystem(void)
{
#ifdef CONFIG_AKIRA_HTTP_SERVER
    int ret = http_server_init();
    if (ret < 0) {
        LOG_WRN("HTTP server initialization failed: %d", ret);
        /* Non-critical */
    }
#endif
    
    return 0;
}

static int init_shell_subsystem(void)
{
#ifdef CONFIG_AKIRA_SHELL
    int ret = akira_shell_init();
    if (ret < 0) {
        LOG_WRN("Shell initialization failed: %d", ret);
        /* Non-critical */
    }
    return 0;
#else
    LOG_DBG("Shell not configured");
    return 0;
#endif
}

int system_manager_init(void)
{
    int ret;
    
    if (sys_state.initialized) {
        return 0;
    }
    
    sys_state.boot_time = k_uptime_get();
    
    LOG_INF("════════════════════════════════════════");
    LOG_INF("       AkiraOS System Manager");
    LOG_INF("════════════════════════════════════════");
    
    /* Phase 1: Initialize event bus (REQUIRED) */
    ret = event_bus_init();
    if (ret < 0) {
        LOG_ERR("Event bus initialization failed: %d", ret);
        return ret;
    }
    
    /* Publish boot event */
    system_event_t boot_event = {
        .type = EVENT_SYSTEM_BOOT,
        .timestamp = sys_state.boot_time
    };
    event_bus_publish(&boot_event);
    
    /* Phase 2: Register all subsystems in init table */
    LOG_INF("Registering subsystems...");
    
    init_table_register("Hardware Manager", INIT_PRIORITY_PLATFORM,
                       hardware_manager_init, true, true);
    
    init_table_register("Network Manager", INIT_PRIORITY_NETWORK,
                       network_manager_init, false, 
                       IS_ENABLED(CONFIG_WIFI) || IS_ENABLED(CONFIG_BT) || 
                       IS_ENABLED(CONFIG_USB_DEVICE_STACK));
    
    init_table_register("Storage", INIT_PRIORITY_STORAGE,
                       init_storage_subsystem, false, 
                       IS_ENABLED(CONFIG_AKIRA_STORAGE_FATFS));
    
    init_table_register("Settings", INIT_PRIORITY_STORAGE,
                       init_settings_subsystem, false, 
                       IS_ENABLED(CONFIG_AKIRA_SETTINGS));
    
    init_table_register("App Subsystem", INIT_PRIORITY_SERVICES,
                       init_app_subsystem, false,
                       IS_ENABLED(CONFIG_AKIRA_APP_MANAGER) || 
                       IS_ENABLED(CONFIG_AKIRA_OCRE_RUNTIME));
    
    init_table_register("Services", INIT_PRIORITY_SERVICES,
                       init_services_subsystem, false,
                       IS_ENABLED(CONFIG_AKIRA_HTTP_SERVER));
    
    init_table_register("Shell", INIT_PRIORITY_LATE,
                       init_shell_subsystem, false, 
                       IS_ENABLED(CONFIG_AKIRA_SHELL));
    
    /* Phase 3: Run initialization table */
    ret = init_table_run();
    if (ret < 0) {
        LOG_ERR("Subsystem initialization failed: %d", ret);
        return ret;
    }
    
    sys_state.initialized = true;
    
    LOG_INF("════════════════════════════════════════");
    LOG_INF("✅ System initialization complete");
    LOG_INF("   Boot time: %llu ms", sys_state.boot_time);
    LOG_INF("════════════════════════════════════════");
    
    return 0;
}

int system_manager_start(void)
{
    if (!sys_state.initialized) {
        LOG_ERR("System not initialized");
        return -EINVAL;
    }
    
    if (sys_state.ready) {
        LOG_WRN("System already started");
        return 0;
    }
    
    LOG_INF("Starting AkiraOS runtime...");
    
    /* Publish system ready event */
    system_event_t ready_event = {
        .type = EVENT_SYSTEM_READY,
        .timestamp = k_uptime_get()
    };
    event_bus_publish(&ready_event);
    
    sys_state.ready = true;
    
    LOG_INF("✅ AkiraOS is ready");
    LOG_INF("════════════════════════════════════════");
    
    return 0;
}

bool system_manager_is_ready(void)
{
    return sys_state.ready;
}

int system_manager_shutdown(void)
{
    LOG_INF("Shutting down AkiraOS...");
    
    system_event_t event = {
        .type = EVENT_SYSTEM_SHUTDOWN,
        .timestamp = k_uptime_get()
    };
    event_bus_publish(&event);
    
    /* Give time for event handlers */
    k_sleep(K_MSEC(100));
    
    LOG_INF("Goodbye!");
    
    sys_reboot(SYS_REBOOT_COLD);
    
    return 0;
}
