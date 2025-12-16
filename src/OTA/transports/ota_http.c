/**
 * @file ota_http.c
 * @brief HTTP OTA Transport for AkiraOS
 *
 * Receives firmware updates via HTTP POST to the web server.
 */

#include "../ota_transport.h"
/* Removed: #include "../ota_manager.h" - Use interface only */
#include "../../connectivity/http/http_server.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ota_http, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    bool enabled;
    ota_transport_state_t state;
    size_t bytes_received;
    size_t total_size;
} http_ota;

/*===========================================================================*/
/* Upload Handler                                                            */
/*===========================================================================*/

static int http_upload_chunk(const uint8_t *data, size_t len,
                             size_t offset, size_t total, void *user_data)
{
    if (!http_ota.enabled)
    {
        return -EINVAL;
    }

    /* Start update if first chunk */
    if (offset == 0)
    {
        http_ota.state = OTA_TRANSPORT_RECEIVING;
        http_ota.total_size = total;
        http_ota.bytes_received = 0;

        enum ota_result ret = ota_start_update(total);
        if (ret != OTA_OK)
        {
            LOG_ERR("Failed to start OTA: %d", ret);
            http_ota.state = OTA_TRANSPORT_ERROR;
            return -1;
        }
    }

    /* Write chunk to OTA manager */
    enum ota_result ret = ota_write_chunk(data, len);
    if (ret != OTA_OK)
    {
        LOG_ERR("Failed to write OTA chunk: %d", ret);
        ota_abort_update();
        http_ota.state = OTA_TRANSPORT_ERROR;
        return -1;
    }

    http_ota.bytes_received += len;

    /* Check if complete */
    if (total > 0 && http_ota.bytes_received >= total)
    {
        ret = ota_finalize_update();
        if (ret != OTA_OK)
        {
            LOG_ERR("Failed to finalize OTA: %d", ret);
            http_ota.state = OTA_TRANSPORT_ERROR;
            return -1;
        }

        http_ota.state = OTA_TRANSPORT_READY;
        LOG_INF("HTTP OTA complete: %zu bytes", http_ota.bytes_received);
    }

    return 0;
}

/*===========================================================================*/
/* Transport Implementation                                                  */
/*===========================================================================*/

static int http_init(void)
{
    if (http_ota.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing HTTP OTA transport");

    memset(&http_ota, 0, sizeof(http_ota));
    http_ota.state = OTA_TRANSPORT_IDLE;
    http_ota.initialized = true;

    return 0;
}

static int http_deinit(void)
{
    http_ota.initialized = false;
    http_ota.enabled = false;
    return 0;
}

static int http_enable(void)
{
    if (!http_ota.initialized)
    {
        return -EINVAL;
    }

    /* Register upload handler with HTTP server */
    int ret = akira_http_register_upload_handler("/api/ota/upload",
                                                 http_upload_chunk, NULL);
    if (ret != 0)
    {
        LOG_ERR("Failed to register upload handler: %d", ret);
        return ret;
    }

    http_ota.enabled = true;
    http_ota.state = OTA_TRANSPORT_READY;

    LOG_INF("HTTP OTA transport enabled");
    return 0;
}

static int http_disable(void)
{
    http_ota.enabled = false;
    http_ota.state = OTA_TRANSPORT_IDLE;

    /* Unregister upload handler */
    akira_http_register_upload_handler(NULL, NULL, NULL);

    LOG_INF("HTTP OTA transport disabled");
    return 0;
}

static bool http_is_available(void)
{
    return akira_http_server_is_running();
}

static bool http_is_active(void)
{
    return http_ota.state == OTA_TRANSPORT_RECEIVING;
}

static int http_abort(void)
{
    if (http_ota.state == OTA_TRANSPORT_RECEIVING)
    {
        ota_abort_update();
        http_ota.state = OTA_TRANSPORT_READY;
    }
    return 0;
}

static ota_transport_state_t http_get_state(void)
{
    return http_ota.state;
}

/*===========================================================================*/
/* Transport Registration                                                    */
/*===========================================================================*/

static const ota_transport_ops_t http_transport = {
    .name = "http",
    .source = OTA_SOURCE_HTTP,
    .init = http_init,
    .deinit = http_deinit,
    .enable = http_enable,
    .disable = http_disable,
    .is_available = http_is_available,
    .is_active = http_is_active,
    .abort = http_abort,
    .get_state = http_get_state};

int ota_http_init(void)
{
    return ota_transport_register(&http_transport);
}
