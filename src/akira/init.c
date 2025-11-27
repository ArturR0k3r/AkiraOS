/**
 * @file init.c
 * @brief AkiraOS System Initialization
 *
 * Handles the initialization sequence for all AkiraOS subsystems.
 */

#include "akira.h"
#include "kernel/service.h"
#include "kernel/event.h"
#include "kernel/process.h"
#include "kernel/memory.h"
#include "kernel/timer.h"
#include "hal/hal.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_init, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Initialization State                                                      */
/*===========================================================================*/

static struct
{
    bool initialized;
    bool running;
    akira_version_t version;
    uint32_t init_time;
} akira_state = {
    .version = {
        .major = AKIRA_VERSION_MAJOR,
        .minor = AKIRA_VERSION_MINOR,
        .patch = AKIRA_VERSION_PATCH}};

/*===========================================================================*/
/* Private Functions                                                         */
/*===========================================================================*/

static int init_kernel_subsystems(void)
{
    int ret;

    LOG_INF("Initializing kernel subsystems...");

    /* Memory must be first */
    ret = akira_memory_init();
    if (ret < 0)
    {
        LOG_ERR("Memory subsystem init failed: %d", ret);
        return ret;
    }

    /* Timer subsystem */
    ret = akira_timer_subsystem_init();
    if (ret < 0)
    {
        LOG_ERR("Timer subsystem init failed: %d", ret);
        return ret;
    }

    /* Service manager */
    ret = akira_service_manager_init();
    if (ret < 0)
    {
        LOG_ERR("Service manager init failed: %d", ret);
        return ret;
    }

    /* Event system */
    ret = akira_event_init();
    if (ret < 0)
    {
        LOG_ERR("Event system init failed: %d", ret);
        return ret;
    }

    /* Process manager */
    ret = akira_process_manager_init();
    if (ret < 0)
    {
        LOG_ERR("Process manager init failed: %d", ret);
        return ret;
    }

    LOG_INF("Kernel subsystems initialized");

    return 0;
}

static int init_hal(void)
{
    LOG_INF("Initializing HAL layer...");

    int ret = akira_core_hal_init();
    if (ret < 0)
    {
        LOG_ERR("HAL init failed: %d", ret);
        return ret;
    }

    LOG_INF("HAL initialized for: %s", akira_hal_platform());

    return 0;
}

static int init_services(void)
{
    LOG_INF("Registering system services...");

    /* Core services will be registered here by their respective modules */
    /* The service manager handles dependency resolution automatically */

    return 0;
}

static void publish_init_event(void)
{
    akira_event_t event = {
        .type = AKIRA_EVENT_SYSTEM_READY,
        .priority = AKIRA_EVENT_PRIORITY_HIGH,
        .timestamp = k_uptime_get_32(),
        .source_id = 0,
        .data = NULL,
        .data_size = 0};

    akira_event_publish(&event);
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int akira_init(void)
{
    int ret;
    uint32_t start_time = k_uptime_get_32();

    if (akira_state.initialized)
    {
        LOG_WRN("AkiraOS already initialized");
        return 0;
    }

    LOG_INF("========================================");
    LOG_INF("  AkiraOS v%d.%d.%d",
            AKIRA_VERSION_MAJOR,
            AKIRA_VERSION_MINOR,
            AKIRA_VERSION_PATCH);
    LOG_INF("  %s", AKIRA_VERSION_STRING);
    LOG_INF("========================================");

    /* Initialize kernel subsystems */
    ret = init_kernel_subsystems();
    if (ret < 0)
    {
        LOG_ERR("Kernel initialization failed");
        return ret;
    }

    /* Initialize HAL */
    ret = init_hal();
    if (ret < 0)
    {
        LOG_ERR("HAL initialization failed");
        return ret;
    }

    /* Register system services */
    ret = init_services();
    if (ret < 0)
    {
        LOG_ERR("Service initialization failed");
        return ret;
    }

    akira_state.init_time = k_uptime_get_32() - start_time;
    akira_state.initialized = true;

    LOG_INF("AkiraOS initialized in %u ms", akira_state.init_time);

    /* Publish init complete event */
    publish_init_event();

    return 0;
}

int akira_start(void)
{
    if (!akira_state.initialized)
    {
        LOG_ERR("AkiraOS not initialized");
        return -1;
    }

    if (akira_state.running)
    {
        LOG_WRN("AkiraOS already running");
        return 0;
    }

    LOG_INF("Starting AkiraOS...");

    /* Start critical services first */
    akira_service_start_all();

    akira_state.running = true;

    LOG_INF("AkiraOS is running");

    return 0;
}

void akira_shutdown(const char *reason)
{
    if (!akira_state.initialized)
    {
        return;
    }

    LOG_INF("Shutting down AkiraOS: %s", reason ? reason : "unknown");

    akira_state.running = false;

    /* Stop all services */
    akira_service_stop_all();

    /* Clean up processes */
    akira_process_cleanup();

    /* Check for memory leaks */
    int leaks = akira_memory_check_leaks();
    if (leaks > 0)
    {
        LOG_WRN("Shutdown with %d memory leaks", leaks);
    }

    akira_state.initialized = false;

    LOG_INF("AkiraOS shutdown complete");
}

bool akira_is_initialized(void)
{
    return akira_state.initialized;
}

bool akira_is_running(void)
{
    return akira_state.running;
}

void akira_version_get(akira_version_t *version)
{
    if (version)
    {
        *version = akira_state.version;
    }
}

const char *akira_version_string(void)
{
    return AKIRA_VERSION_STRING;
}

uint32_t akira_init_time(void)
{
    return akira_state.init_time;
}

void akira_print_banner(void)
{
    printk("\n");
    printk("    _    _    _           ___  ____  \n");
    printk("   / \\  | | _(_)_ __ __ _/ _ \\/ ___| \n");
    printk("  / _ \\ | |/ / | '__/ _` | | | \\___ \\ \n");
    printk(" / ___ \\|   <| | | | (_| | |_| |___) |\n");
    printk("/_/   \\_\\_|\\_\\_|_|  \\__,_|\\___/|____/ \n");
    printk("\n");
    printk("  Version: %s\n", AKIRA_VERSION_STRING);
    printk("  Platform: %s\n", akira_hal_platform());
    printk("\n");
}

void akira_print_status(void)
{
    LOG_INF("=== AkiraOS Status ===");
    LOG_INF("Version: %s", AKIRA_VERSION_STRING);
    LOG_INF("Platform: %s", akira_hal_platform());
    LOG_INF("Initialized: %s", akira_state.initialized ? "yes" : "no");
    LOG_INF("Running: %s", akira_state.running ? "yes" : "no");
    LOG_INF("Init time: %u ms", akira_state.init_time);
    LOG_INF("Uptime: %u sec", akira_uptime_sec());
    LOG_INF("Active services: %d", akira_service_count());
    LOG_INF("Active processes: %d", akira_process_count());
    LOG_INF("Active timers: %d", akira_timer_count());

    akira_memory_dump();
}
