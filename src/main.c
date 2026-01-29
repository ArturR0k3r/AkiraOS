/**
 * @file main.c
 * @brief AkiraOS Main Entry Point
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/platform_hal.h>
#include <runtime/akira_runtime.h>
#include <runtime/app_loader/loader.h>
#ifdef CONFIG_FILE_SYSTEM
#include <storage/fs_manager.h>
#endif
#ifdef CONFIG_AKIRA_APP_MANAGER
#include <runtime/app_manager/app_manager.h>
#endif
#include "settings/settings.h"
#include "drivers/psram.h"

LOG_MODULE_REGISTER(akira_main, CONFIG_AKIRA_LOG_LEVEL);

int main(void)
{
    LOG_INF("AkiraOS booting (Minimalist v1.4.x)");

    /* Initialize hardware HAL */
    if (akira_hal_init() < 0) {
        LOG_ERR("HAL init failed");
        return -ENODEV;
    }


#ifdef CONFIG_FILE_SYSTEM
    /* Ensure filesystem is initialized for storage/backing */
    if (fs_manager_init() < 0) {
        LOG_WRN("Filesystem init failed - continuing without persistent storage");
    }
#endif

#ifdef CONFIG_AKIRA_PSRAM
    /* Initialize PSRAM heap */
    printk("PSRAM size: %d bytes\n", esp_psram_get_size());
    if( akira_init_psram_heap() < 0 ){
        LOG_ERR("PSRAM heap init failed");
    }
    else{
        LOG_INF("PSRAM heap initialized");
    }
#endif  

#ifdef CONFIG_AKIRA_SETTINGS
    /* Initialize settings subsystem */
    if (akira_settings_init() < 0) {
        LOG_WRN("Settings init failed - continuing without settings support");
    }
    else{
        LOG_INF("Settings subsystem initialized");
    }
    
#endif

    /* Initialize runtime */
    if (akira_runtime_init() < 0) {
        LOG_ERR("Runtime init failed");
        return -EIO;
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    app_manager_init();
#endif

/* Self-test (native_sim): install a dummy WASM and optional manifest */
static const uint8_t dummy_wasm[] = {0x00, 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00};
const char *manifest = "{\"capabilities\":[\"display.write\",\"input.read\"]}";
int sid = app_loader_install_with_manifest("selftest", dummy_wasm, sizeof(dummy_wasm), manifest, strlen(manifest));
if (sid >= 0) {
    LOG_INF("Selftest installed as slot %d", sid);
    if (akira_runtime_start(sid) == 0) {
        LOG_INF("Selftest started (slot %d)", sid);
    } else {
        LOG_WRN("Selftest start failed (slot %d)", sid);
    }
} else {
    LOG_WRN("Selftest install failed: %d", sid);
}
#ifdef CONFIG_AKIRA_SELFTEST
#endif

    LOG_INF("AkiraOS init complete");

    /* Idle loop */
    while (1) {
        k_sleep(K_SECONDS(10));
    }

    return 0;
}
