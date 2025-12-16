/**
 * @file main.c
 * @brief AkiraOS Main Entry Point
 * 
 * Simplified main using the new system manager architecture.
 * All subsystem initialization is delegated to the system manager.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include "core/system_manager.h"

LOG_MODULE_REGISTER(akira_main, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @brief Main entry point for AkiraOS
 * 
 * Delegates all initialization to the system manager which orchestrates:
 * - Event bus initialization
 * - Hardware manager (HAL, drivers, display, buttons)
 * - Network manager (WiFi, Bluetooth, USB)
 * - Storage and settings
 * - Application services (OCRE, WASM, OTA)
 * - Shell and other services
 */
int main(void)
{
    int ret;
    
    printk("\n");
    printk("════════════════════════════════════════\n");
    printk("          AkiraOS v1.3.0\n");
    printk("   Modular Embedded Operating System\n");
    printk("════════════════════════════════════════\n");
    printk("\n");
    
    LOG_INF("Build: %s %s", __DATE__, __TIME__);
    
    /* Initialize system manager - handles all subsystems */
    ret = system_manager_init();
    if (ret < 0) {
        LOG_ERR("System initialization failed: %d", ret);
        LOG_ERR("System cannot continue - halting");
        return ret;
    }
    
    /* Start system runtime */
    ret = system_manager_start();
    if (ret < 0) {
        LOG_ERR("System start failed: %d", ret);
        return ret;
    }
    
    /* Main loop - system is now running */
    uint32_t loop_count = 0;
    while (1) {
        loop_count++;
        
        /* Periodic heartbeat every 60 seconds */
        if (loop_count % 6 == 0) {
            uint64_t uptime = k_uptime_get() / 1000;
            LOG_INF("💓 Heartbeat: uptime=%llus, loops=%u", uptime, loop_count);
        }
        
        k_sleep(K_SECONDS(10));
    }
    
    return 0;
}

