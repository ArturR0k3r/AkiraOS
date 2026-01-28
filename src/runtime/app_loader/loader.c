#include "loader.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdlib.h>
#include <runtime/akira_runtime.h>

LOG_MODULE_REGISTER(app_loader, CONFIG_AKIRA_LOG_LEVEL);

static app_loader_provider_cb_t g_provider = NULL;
static void *g_provider_ctx = NULL;

/* Simple assembly buffer for chunked installs */
static uint8_t *g_assembly_buf = NULL;
static size_t g_assembly_size = 0;

int app_loader_init(void)
{
    /* Nothing heavy for now */
    return 0;
}

int app_loader_register_provider(app_loader_provider_cb_t cb, void *ctx)
{
    g_provider = cb;
    g_provider_ctx = ctx;
    LOG_INF("App loader provider registered: %p", cb);
    return 0;
}

int app_loader_receive_chunk(const uint8_t *chunk, size_t len, bool final)
{
    if (!chunk || len == 0) {
        return -EINVAL;
    }

    uint8_t *nbuf = k_malloc(g_assembly_size + len);
    if (!nbuf) return -ENOMEM;

    if (g_assembly_buf) {
        memcpy(nbuf, g_assembly_buf, g_assembly_size);
        k_free(g_assembly_buf);
    }
    memcpy(nbuf + g_assembly_size, chunk, len);
    g_assembly_buf = nbuf;
    g_assembly_size += len;

    if (final) {
        /* final chunk - load into runtime */
        int id = akira_runtime_load_wasm(g_assembly_buf, (uint32_t)g_assembly_size);
        k_free(g_assembly_buf);
        g_assembly_buf = NULL;
        g_assembly_size = 0;
        if (id < 0) {
            LOG_ERR("Failed to load assembled WASM: %d", id);
            return id;
        }
        LOG_INF("WASM installed into slot %d", id);
        return id;
    }

    return (int)g_assembly_size;
}

int app_loader_install_memory(const char *name, const void *binary, size_t size)
{
    (void)name;
    if (!binary || size == 0) return -EINVAL;
    return akira_runtime_load_wasm((const uint8_t *)binary, (uint32_t)size);
}