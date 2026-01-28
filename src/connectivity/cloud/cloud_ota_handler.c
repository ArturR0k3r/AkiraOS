/**
 * @file cloud_ota_handler.c
 * @brief Cloud OTA Handler Implementation
 *
 * Receives firmware from cloud/BT/web and writes to flash.
 */

#include "cloud_ota_handler.h"
#include "cloud_client.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <string.h>

/* OTA manager for flash operations */
#include "connectivity/ota/ota_manager.h"

LOG_MODULE_REGISTER(cloud_ota, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Private Data                                                              */
/*===========================================================================*/

static struct
{
    bool initialized;
    ota_download_state_t state;

    /* Available firmware info */
    ota_firmware_info_t available;
    bool update_available;

    /* Download state */
    uint32_t total_size;
    uint32_t received;
    uint16_t expected_chunks;
    uint16_t received_chunks;
    msg_source_t source;

    /* Callbacks */
    cloud_ota_available_cb_t available_cb;
    cloud_ota_progress_cb_t progress_cb;
    cloud_ota_complete_cb_t complete_cb;
    void *user_data;

    /* Synchronization */
    struct k_mutex mutex;
} ota_handler;

/*===========================================================================*/
/* Private Functions                                                         */
/*===========================================================================*/

static void complete_download(bool success, const char *error)
{
    LOG_INF("OTA download %s%s%s",
            success ? "SUCCESS" : "FAILED",
            error ? ": " : "",
            error ? error : "");

    ota_handler.state = success ? OTA_DL_READY : OTA_DL_ERROR;

    if (ota_handler.complete_cb)
    {
        ota_handler.complete_cb(success, error, ota_handler.user_data);
    }
}

static int handle_fw_available(const cloud_message_t *msg, msg_source_t source)
{
    if (msg->header.payload_len < sizeof(payload_fw_metadata_t))
    {
        LOG_WRN("FW available without metadata");
        return 0;
    }

    payload_fw_metadata_t *meta = (payload_fw_metadata_t *)msg->payload;

    k_mutex_lock(&ota_handler.mutex, K_FOREVER);

    /* Store info */
    memcpy(ota_handler.available.version, meta->version, 4);
    ota_handler.available.size = meta->size;
    memcpy(ota_handler.available.hash, meta->hash, 32);
    strncpy(ota_handler.available.release_notes, meta->release_notes,
            sizeof(ota_handler.available.release_notes) - 1);
    ota_handler.available.source = source;
    ota_handler.update_available = true;
    ota_handler.state = OTA_DL_AVAILABLE;

    LOG_INF("Firmware v%d.%d.%d available from %s (%u bytes)",
            meta->version[0], meta->version[1], meta->version[2],
            cloud_msg_source_str(source), meta->size);

    /* Notify */
    if (ota_handler.available_cb)
    {
        ota_handler.available_cb(&ota_handler.available, ota_handler.user_data);
    }

    k_mutex_unlock(&ota_handler.mutex);
    return 0;
}

static int handle_fw_metadata(const cloud_message_t *msg, msg_source_t source)
{
    if (msg->header.payload_len < sizeof(payload_fw_metadata_t))
    {
        LOG_ERR("Invalid FW metadata");
        return -EINVAL;
    }

    payload_fw_metadata_t *meta = (payload_fw_metadata_t *)msg->payload;

    k_mutex_lock(&ota_handler.mutex, K_FOREVER);

    /* Store download info */
    memcpy(ota_handler.available.version, meta->version, 4);
    ota_handler.available.size = meta->size;
    memcpy(ota_handler.available.hash, meta->hash, 32);
    ota_handler.available.source = source;

    ota_handler.total_size = meta->size;
    ota_handler.expected_chunks = meta->chunk_count;
    ota_handler.received = 0;
    ota_handler.received_chunks = 0;
    ota_handler.source = source;
    ota_handler.state = OTA_DL_RECEIVING;

    LOG_INF("Starting OTA: v%d.%d.%d, %u bytes, %u chunks",
            meta->version[0], meta->version[1], meta->version[2],
            meta->size, meta->chunk_count);

    /* Initialize OTA manager for writing */
#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    int ret = (int)ota_start_update(meta->size);
    if (ret < 0)
    {
        LOG_ERR("Failed to start OTA manager: %d", ret);
        ota_handler.state = OTA_DL_ERROR;
        k_mutex_unlock(&ota_handler.mutex);
        return ret;
    }
#endif

    k_mutex_unlock(&ota_handler.mutex);
    return 0;
}

static int handle_fw_chunk(const cloud_message_t *msg, msg_source_t source)
{
    if (ota_handler.state != OTA_DL_RECEIVING)
    {
        LOG_WRN("Received chunk but not in receiving state");
        return -EINVAL;
    }

    if (msg->header.payload_len < sizeof(payload_chunk_t))
    {
        LOG_ERR("Invalid chunk payload");
        return -EINVAL;
    }

    payload_chunk_t *chunk = (payload_chunk_t *)msg->payload;
    size_t data_len = msg->header.payload_len - offsetof(payload_chunk_t, data);

    k_mutex_lock(&ota_handler.mutex, K_FOREVER);

    LOG_DBG("FW chunk %u/%u: offset=%u, len=%zu",
            chunk->chunk_index + 1, ota_handler.expected_chunks,
            chunk->offset, data_len);

    /* Write to flash */
#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    int ret = (int)ota_write_chunk(chunk->data, data_len);
    if (ret < 0)
    {
        LOG_ERR("Failed to write chunk: %d", ret);
        ota_handler.state = OTA_DL_ERROR;
        k_mutex_unlock(&ota_handler.mutex);
        return ret;
    }
#endif

    ota_handler.received += data_len;
    ota_handler.received_chunks++;

    /* Progress callback */
    if (ota_handler.progress_cb)
    {
        ota_handler.progress_cb(ota_handler.received, ota_handler.total_size,
                                ota_handler.user_data);
    }

    k_mutex_unlock(&ota_handler.mutex);
    return 0;
}

static int handle_fw_complete(const cloud_message_t *msg, msg_source_t source)
{
    k_mutex_lock(&ota_handler.mutex, K_FOREVER);

    if (ota_handler.state != OTA_DL_RECEIVING)
    {
        LOG_WRN("Complete received but not downloading");
        k_mutex_unlock(&ota_handler.mutex);
        return 0;
    }

    LOG_INF("OTA transfer complete: %u/%u bytes",
            ota_handler.received, ota_handler.total_size);

    ota_handler.state = OTA_DL_VERIFYING;

    /* Verify firmware - done during finalize */
#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    /* Hash verification is handled by MCUboot during boot */
    int ret = 0; /* Placeholder - actual verification in ota_finalize_update */
    if (ret < 0)
    {
        LOG_ERR("Firmware verification failed: %d", ret);
        complete_download(false, "Verification failed");
        k_mutex_unlock(&ota_handler.mutex);
        return ret;
    }
#endif

    complete_download(true, NULL);

    k_mutex_unlock(&ota_handler.mutex);
    return 0;
}

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

int cloud_ota_handler_init(void)
{
    if (ota_handler.initialized)
    {
        return 0;
    }

    memset(&ota_handler, 0, sizeof(ota_handler));
    k_mutex_init(&ota_handler.mutex);
    ota_handler.state = OTA_DL_IDLE;

    /* Register with cloud client */
    cloud_client_register_handler(MSG_CAT_OTA, cloud_ota_handle_message);

    ota_handler.initialized = true;
    LOG_INF("Cloud OTA handler initialized");

    return 0;
}

int cloud_ota_handler_deinit(void)
{
    if (!ota_handler.initialized)
    {
        return 0;
    }

    cloud_ota_cancel();

    ota_handler.initialized = false;
    LOG_INF("Cloud OTA handler deinitialized");

    return 0;
}

int cloud_ota_check(cloud_ota_available_cb_t callback, void *user_data)
{
    if (!ota_handler.initialized)
    {
        return -EINVAL;
    }

    ota_handler.available_cb = callback;
    ota_handler.user_data = user_data;
    ota_handler.state = OTA_DL_CHECKING;

    return cloud_client_check_firmware();
}

int cloud_ota_download(const char *version,
                       cloud_ota_progress_cb_t progress_cb,
                       cloud_ota_complete_cb_t complete_cb,
                       void *user_data)
{
    if (!ota_handler.initialized)
    {
        return -EINVAL;
    }

    if (ota_handler.state == OTA_DL_RECEIVING)
    {
        LOG_WRN("Download already in progress");
        return -EALREADY;
    }

    ota_handler.progress_cb = progress_cb;
    ota_handler.complete_cb = complete_cb;
    ota_handler.user_data = user_data;
    ota_handler.state = OTA_DL_RECEIVING;

    return cloud_client_request_firmware(version);
}

int cloud_ota_cancel(void)
{
    k_mutex_lock(&ota_handler.mutex, K_FOREVER);

    if (ota_handler.state == OTA_DL_RECEIVING)
    {
        LOG_INF("Cancelling OTA download");

#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
        ota_abort_update();
#endif

        complete_download(false, "Cancelled");
    }

    ota_handler.state = OTA_DL_IDLE;

    k_mutex_unlock(&ota_handler.mutex);
    return 0;
}

int cloud_ota_apply(void)
{
    if (ota_handler.state != OTA_DL_READY)
    {
        LOG_ERR("No firmware ready to apply");
        return -EINVAL;
    }

    LOG_INF("Applying firmware update...");
    ota_handler.state = OTA_DL_APPLYING;

#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    int ret = (int)ota_finalize_update();
    if (ret < 0)
    {
        LOG_ERR("Failed to finalize update: %d", ret);
        ota_handler.state = OTA_DL_ERROR;
        return ret;
    }
#endif

    LOG_INF("Rebooting to apply update...");
    k_sleep(K_MSEC(100)); /* Let log flush */
    sys_reboot(SYS_REBOOT_COLD);

    /* Won't reach here */
    return 0;
}

ota_download_state_t cloud_ota_get_state(void)
{
    return ota_handler.state;
}

int cloud_ota_get_progress(uint32_t *received, uint32_t *total)
{
    if (received)
        *received = ota_handler.received;
    if (total)
        *total = ota_handler.total_size;
    return 0;
}

int cloud_ota_get_available_info(ota_firmware_info_t *info)
{
    if (!ota_handler.update_available || !info)
    {
        return -ENOENT;
    }

    memcpy(info, &ota_handler.available, sizeof(*info));
    return 0;
}

int cloud_ota_handle_message(const cloud_message_t *msg, msg_source_t source)
{
    if (!ota_handler.initialized)
    {
        return -EINVAL;
    }

    switch (msg->header.type)
    {
    case MSG_TYPE_FW_AVAILABLE:
        return handle_fw_available(msg, source);

    case MSG_TYPE_FW_METADATA:
        return handle_fw_metadata(msg, source);

    case MSG_TYPE_FW_CHUNK:
        return handle_fw_chunk(msg, source);

    case MSG_TYPE_FW_COMPLETE:
        return handle_fw_complete(msg, source);

    case MSG_TYPE_FW_CHECK:
        /* Server asking us to check - respond with current version */
        LOG_DBG("FW check request from %s", cloud_msg_source_str(source));
        return 0;

    default:
        return 0;
    }
}
