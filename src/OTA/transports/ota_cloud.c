/**
 * @file ota_cloud.c
 * @brief Cloud OTA Transport for AkiraOS (Stub)
 *
 * Future: Receives firmware updates from AkiraHub cloud service.
 */

#include "../ota_transport.h"
#include "../ota_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ota_cloud, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    bool enabled;
    ota_transport_state_t state;

    /* Cloud connection state */
    bool connected;
    char server_url[128];
    char device_id[64];
    char auth_token[128];
} cloud_ota;

/*===========================================================================*/
/* Cloud Protocol (Stub)                                                     */
/*===========================================================================*/

/* Future implementation will:
 * 1. Connect to AkiraHub server via HTTPS/MQTT
 * 2. Check for available firmware updates
 * 3. Download firmware in chunks
 * 4. Verify signature and apply update
 */

/*===========================================================================*/
/* Transport Implementation                                                  */
/*===========================================================================*/

static int cloud_init(void)
{
    if (cloud_ota.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing Cloud OTA transport (stub)");

    memset(&cloud_ota, 0, sizeof(cloud_ota));
    cloud_ota.state = OTA_TRANSPORT_IDLE;

    /* Default cloud server */
    strncpy(cloud_ota.server_url, "https://ota.akirahub.io",
            sizeof(cloud_ota.server_url) - 1);

    cloud_ota.initialized = true;
    return 0;
}

static int cloud_deinit(void)
{
    cloud_ota.initialized = false;
    cloud_ota.enabled = false;
    return 0;
}

static int cloud_enable(void)
{
    if (!cloud_ota.initialized)
    {
        return -EINVAL;
    }

    /* TODO: Connect to cloud server */
    LOG_WRN("Cloud OTA not yet implemented");

    cloud_ota.enabled = true;
    cloud_ota.state = OTA_TRANSPORT_READY;

    return 0;
}

static int cloud_disable(void)
{
    cloud_ota.enabled = false;
    cloud_ota.state = OTA_TRANSPORT_IDLE;
    cloud_ota.connected = false;

    return 0;
}

static bool cloud_is_available(void)
{
    /* TODO: Check network and cloud connectivity */
    return cloud_ota.connected;
}

static bool cloud_is_active(void)
{
    return cloud_ota.state == OTA_TRANSPORT_RECEIVING;
}

static int cloud_abort(void)
{
    if (cloud_ota.state == OTA_TRANSPORT_RECEIVING)
    {
        ota_abort_update();
        cloud_ota.state = OTA_TRANSPORT_READY;
    }
    return 0;
}

static ota_transport_state_t cloud_get_state(void)
{
    return cloud_ota.state;
}

/*===========================================================================*/
/* Transport Registration                                                    */
/*===========================================================================*/

static const ota_transport_ops_t cloud_transport = {
    .name = "cloud",
    .source = OTA_SOURCE_CLOUD,
    .init = cloud_init,
    .deinit = cloud_deinit,
    .enable = cloud_enable,
    .disable = cloud_disable,
    .is_available = cloud_is_available,
    .is_active = cloud_is_active,
    .abort = cloud_abort,
    .get_state = cloud_get_state};

int ota_cloud_init(void)
{
    return ota_transport_register(&cloud_transport);
}

/*===========================================================================*/
/* Cloud Configuration API (for future use)                                  */
/*===========================================================================*/

int ota_cloud_set_server(const char *url)
{
    if (!url)
    {
        return -EINVAL;
    }
    strncpy(cloud_ota.server_url, url, sizeof(cloud_ota.server_url) - 1);
    return 0;
}

int ota_cloud_set_credentials(const char *device_id, const char *auth_token)
{
    if (device_id)
    {
        strncpy(cloud_ota.device_id, device_id, sizeof(cloud_ota.device_id) - 1);
    }
    if (auth_token)
    {
        strncpy(cloud_ota.auth_token, auth_token, sizeof(cloud_ota.auth_token) - 1);
    }
    return 0;
}

int ota_cloud_check_update(void)
{
    LOG_WRN("Cloud update check not implemented");
    return -ENOTSUP;
}
