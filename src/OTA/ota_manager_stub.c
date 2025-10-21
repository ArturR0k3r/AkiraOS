/**
 * @file ota_manager_stub.c
 * @brief OTA Manager Stub Implementation for platforms without Flash/MCUboot
 *
 * Provides no-op implementations for OTA functions on native_sim and other
 * platforms that don't have flash or MCUboot support.
 */

#include "ota_manager.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>

LOG_MODULE_REGISTER(ota_manager_stub, LOG_LEVEL_INF);

/* Stub OTA state */
static struct ota_progress stub_progress = {
    .state = OTA_STATE_IDLE,
    .total_size = 0,
    .bytes_written = 0,
    .percentage = 0,
    .last_error = OTA_OK,
    .status_message = "OTA not available on this platform"};

int ota_manager_init(void)
{
    LOG_INF("OTA Manager (stub mode - no flash support)");
    return 0;
}

enum ota_result ota_start_update(size_t expected_size)
{
    LOG_WRN("OTA not available on this platform");
    return OTA_ERROR_NOT_INITIALIZED;
}

enum ota_result ota_write_chunk(const uint8_t *data, size_t length)
{
    return OTA_ERROR_NOT_INITIALIZED;
}

enum ota_result ota_finalize_update(void)
{
    return OTA_ERROR_NOT_INITIALIZED;
}

enum ota_result ota_abort_update(void)
{
    return OTA_OK;
}

const struct ota_progress *ota_get_progress(void)
{
    return &stub_progress;
}

enum ota_result ota_confirm_firmware(void)
{
    return OTA_OK;
}

enum ota_result ota_register_progress_callback(ota_progress_cb_t callback, void *user_data)
{
    return OTA_OK;
}

bool ota_is_update_in_progress(void)
{
    return false;
}

const char *ota_result_to_string(enum ota_result result)
{
    return "OTA not available";
}

const char *ota_state_to_string(enum ota_state state)
{
    switch (state)
    {
    case OTA_STATE_IDLE:
        return "Idle";
    case OTA_STATE_RECEIVING:
        return "Receiving";
    case OTA_STATE_VALIDATING:
        return "Validating";
    case OTA_STATE_INSTALLING:
        return "Installing";
    case OTA_STATE_COMPLETE:
        return "Complete";
    case OTA_STATE_ERROR:
        return "Error";
    default:
        return "Unknown";
    }
}

void ota_reboot_to_apply_update(uint32_t delay_ms)
{
    LOG_INF("OTA reboot not available on this platform");
}
