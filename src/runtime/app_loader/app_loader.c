#include "app_loader.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdlib.h>
#include <runtime/akira_runtime.h>
#include <connectivity/transport_interface.h>

LOG_MODULE_REGISTER(app_loader, CONFIG_AKIRA_LOG_LEVEL);

static app_loader_provider_cb_t g_provider = NULL;
static void *g_provider_ctx = NULL;

/* Chunked file writing state */
static struct {
    uint8_t *buffer;          /* Assembly buffer for chunked writes */
    size_t buffer_size;       /* Allocated buffer size */
    size_t bytes_received;    /* Bytes received so far */
    size_t total_expected;    /* Total expected size */
    char app_name[32];        /* Application name */
    bool transfer_active;     /* Transfer in progress flag */
} wasm_transfer;

static K_MUTEX_DEFINE(app_loader_mutex);
static int wasm_handler_id = -1;

/* Forward declaration for transport callback */
static int wasm_data_callback(const uint8_t *data, size_t len,
                              const struct transport_chunk_info *info);

/**
 * @brief Transport callback for WASM application data
 *
 * Handles chunked file writing with buffer management.
 * Implements zero-copy where possible, assembles chunks for WASM loading.
 */
static int wasm_data_callback(const uint8_t *data, size_t len,
                              const struct transport_chunk_info *info)
{
    if (!info) {
        return -EINVAL;
    }

    k_mutex_lock(&app_loader_mutex, K_FOREVER);

    /* Handle transfer start */
    if (info->flags & TRANSPORT_FLAG_CHUNK_START) {
        LOG_INF("WASM transfer started: size=%u, name=%s",
                info->total_size, info->name ? info->name : "(null)");

        /* Clean up any previous incomplete transfer */
        if (wasm_transfer.buffer) {
            k_free(wasm_transfer.buffer);
            wasm_transfer.buffer = NULL;
        }

        /* Pre-allocate buffer if size is known */
        if (info->total_size > 0) {
            wasm_transfer.buffer = k_malloc(info->total_size);
            if (!wasm_transfer.buffer) {
                LOG_ERR("Failed to allocate WASM buffer (%u bytes)", info->total_size);
                k_mutex_unlock(&app_loader_mutex);
                return -ENOMEM;
            }
            wasm_transfer.buffer_size = info->total_size;
        } else {
            wasm_transfer.buffer_size = 0;
        }

        wasm_transfer.bytes_received = 0;
        wasm_transfer.total_expected = info->total_size;
        wasm_transfer.transfer_active = true;

        /* Store app name */
        if (info->name) {
            strncpy(wasm_transfer.app_name, info->name, sizeof(wasm_transfer.app_name) - 1);
            wasm_transfer.app_name[sizeof(wasm_transfer.app_name) - 1] = '\0';
        } else {
            strcpy(wasm_transfer.app_name, "wasm_app");
        }

        k_mutex_unlock(&app_loader_mutex);
        return 0;
    }

    /* Handle abort */
    if (info->flags & TRANSPORT_FLAG_ABORT) {
        LOG_WRN("WASM transfer aborted");
        if (wasm_transfer.buffer) {
            k_free(wasm_transfer.buffer);
            wasm_transfer.buffer = NULL;
        }
        wasm_transfer.buffer_size = 0;
        wasm_transfer.bytes_received = 0;
        wasm_transfer.transfer_active = false;
        k_mutex_unlock(&app_loader_mutex);
        return 0;
    }

    /* Handle transfer end - load the WASM */
    if (info->flags & TRANSPORT_FLAG_CHUNK_END) {
        LOG_INF("WASM transfer complete: %zu bytes received", wasm_transfer.bytes_received);

        if (!wasm_transfer.buffer || wasm_transfer.bytes_received == 0) {
            LOG_ERR("No WASM data to load");
            k_mutex_unlock(&app_loader_mutex);
            return -ENODATA;
        }

        /* Load into runtime */
        int app_id = akira_runtime_install_with_manifest(
            wasm_transfer.app_name,
            wasm_transfer.buffer,
            wasm_transfer.bytes_received,
            NULL, 0
        );

        /* Clean up */
        k_free(wasm_transfer.buffer);
        wasm_transfer.buffer = NULL;
        wasm_transfer.buffer_size = 0;
        wasm_transfer.bytes_received = 0;
        wasm_transfer.transfer_active = false;

        k_mutex_unlock(&app_loader_mutex);

        if (app_id < 0) {
            LOG_ERR("Failed to load WASM: %d", app_id);
            return -EIO;
        }

        LOG_INF("WASM app '%s' loaded, id=%d", wasm_transfer.app_name, app_id);
        return 0;
    }

    /* Handle data chunks */
    if (data && len > 0) {
        if (!wasm_transfer.transfer_active) {
            LOG_ERR("Received data without active transfer");
            k_mutex_unlock(&app_loader_mutex);
            return -EINVAL;
        }

        /* Grow buffer if needed (for unknown size transfers) */
        size_t needed = wasm_transfer.bytes_received + len;
        if (needed > wasm_transfer.buffer_size) {
            /* Allocate with some headroom */
            size_t new_size = MAX(needed, wasm_transfer.buffer_size + 4096);
            uint8_t *new_buf = k_malloc(new_size);
            if (!new_buf) {
                LOG_ERR("Failed to grow WASM buffer to %zu", new_size);
                k_mutex_unlock(&app_loader_mutex);
                return -ENOMEM;
            }

            if (wasm_transfer.buffer && wasm_transfer.bytes_received > 0) {
                memcpy(new_buf, wasm_transfer.buffer, wasm_transfer.bytes_received);
                k_free(wasm_transfer.buffer);
            }
            wasm_transfer.buffer = new_buf;
            wasm_transfer.buffer_size = new_size;
        }

        /* Copy chunk to buffer */
        memcpy(wasm_transfer.buffer + wasm_transfer.bytes_received, data, len);
        wasm_transfer.bytes_received += len;

        LOG_DBG("WASM chunk: %zu bytes, total=%zu/%zu",
                len, wasm_transfer.bytes_received, wasm_transfer.total_expected);

        k_mutex_unlock(&app_loader_mutex);
        return 0;
    }

    k_mutex_unlock(&app_loader_mutex);
    return 0;
}

int app_loader_init(void)
{
    /* Initialize transport interface */
    transport_init();

    /* Register for WASM_APP data type */
    wasm_handler_id = transport_register_handler(
        TRANSPORT_DATA_WASM_APP,
        wasm_data_callback,
        NULL,
        0  /* Highest priority */
    );

    if (wasm_handler_id < 0) {
        LOG_ERR("Failed to register WASM transport handler: %d", wasm_handler_id);
        /* Continue anyway - direct API still works */
    } else {
        LOG_INF("WASM transport handler registered (id=%d)", wasm_handler_id);
    }

    /* Initialize transfer state */
    memset(&wasm_transfer, 0, sizeof(wasm_transfer));

    LOG_INF("App loader initialized");
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

    /* Use transport layer for chunked receives */
    struct transport_chunk_info info = {
        .type = TRANSPORT_DATA_WASM_APP,
        .total_size = 0,  /* Unknown */
        .offset = wasm_transfer.bytes_received,
        .flags = final ? TRANSPORT_FLAG_CHUNK_END : 0,
        .name = wasm_transfer.app_name[0] ? wasm_transfer.app_name : NULL,
    };

    /* Start transfer if not active */
    if (!wasm_transfer.transfer_active && !final) {
        struct transport_chunk_info start_info = info;
        start_info.flags = TRANSPORT_FLAG_CHUNK_START;
        int ret = wasm_data_callback(NULL, 0, &start_info);
        if (ret != 0) {
            return ret;
        }
    }

    /* Process chunk */
    int ret = wasm_data_callback(chunk, len, &info);
    if (ret != 0) {
        return ret;
    }

    if (final) {
        /* Signal end */
        struct transport_chunk_info end_info = info;
        end_info.flags = TRANSPORT_FLAG_CHUNK_END;
        return wasm_data_callback(NULL, 0, &end_info);
    }

    return (int)wasm_transfer.bytes_received;
}

int app_loader_install_memory(const char *name, const void *binary, size_t size)
{
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