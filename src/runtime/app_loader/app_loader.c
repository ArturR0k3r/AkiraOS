#include "app_loader.h"
#include <runtime/akira_runtime.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_loader, CONFIG_AKIRA_LOG_LEVEL);

int app_loader_init(void)
{
    /* Minimal loader - no persistent storage for MVP */
    return 0;
}

int app_loader_install_memory(const char *name, const void *binary, size_t size)
{
    /* Default to persistent install if name provided */
    if (!binary || size == 0) return -EINVAL;
    if (name && strlen(name) > 0) {
        return akira_runtime_install_with_manifest(name, binary, size, NULL, 0);
    }
    return akira_runtime_load_wasm((const uint8_t *)binary, (uint32_t)size);
}

int app_loader_install_with_manifest(const char *name, const void *binary, size_t size, const char *manifest_json, size_t manifest_size)
{
    if (!name || !binary) return -EINVAL;
    return akira_runtime_install_with_manifest(name, binary, size, manifest_json, manifest_size);
}
