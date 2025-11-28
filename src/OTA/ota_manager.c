/**
 * @file ota_manager.c
 * @brief OTA Manager Implementation
 * Optimized OTA manager for ESP32 device with MCUboot integration
 * Handles firmware updates, validation, and rollback functionality
 * with reduced memory footprint and improved performance.
 */

#include "ota_manager.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdlib.h>
#include "akira/akira.h"

/* Include flash and MCUboot APIs only if available */
#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/mcuboot.h>
#define OTA_FLASH_AVAILABLE 1
#else
#define OTA_FLASH_AVAILABLE 0
#endif

LOG_MODULE_REGISTER(ota_manager, AKIRA_LOG_LEVEL);

#if OTA_FLASH_AVAILABLE
#define FLASH_AREA_IMAGE_PRIMARY FIXED_PARTITION_ID(slot0_partition)
#define FLASH_AREA_IMAGE_SECONDARY FIXED_PARTITION_ID(slot1_partition)
#endif

/* Optimized buffer sizes */
#define OTA_WRITE_BUFFER_SIZE 4096        // Align with flash page size
#define OTA_PROGRESS_REPORT_INTERVAL 8192 // Report every 8KB instead of calculating modulo

/* Thread stack - reduced from default */
static K_THREAD_STACK_DEFINE(ota_thread_stack, 3072); // Reduced to save memory
static struct k_thread ota_thread_data;
static k_tid_t ota_thread_id;

/* Compact OTA state management */
static struct
{
    enum ota_state state;
    enum ota_result last_error;
    uint32_t total_size;
    uint32_t bytes_written;
    uint32_t last_progress_report;
    uint8_t percentage;
    char status_message[64]; // Reduced from larger buffer
} ota_status __aligned(4);

static K_MUTEX_DEFINE(ota_mutex);

/* Flash management */
static const struct flash_area *secondary_fa = NULL;
static uint8_t write_buffer[OTA_WRITE_BUFFER_SIZE] __aligned(4);
static uint16_t buffer_pos = 0;

/* Modular OTA transport support */
#define MAX_OTA_TRANSPORTS 4
static const ota_transport_t *ota_transports[MAX_OTA_TRANSPORTS];
static int ota_transport_count = 0;

/* Progress callback (restored for compatibility) */
typedef void (*ota_progress_cb_t)(const struct ota_progress *progress, void *user_data);
static ota_progress_cb_t progress_callback = NULL;
static void *callback_user_data = NULL;

int ota_manager_register_transport(const ota_transport_t *transport)
{
    if (!transport || !transport->name)
        return OTA_ERROR_INVALID_PARAM;
    if (ota_transport_count >= MAX_OTA_TRANSPORTS)
        return OTA_ERROR_INSUFFICIENT_SPACE;
    for (int i = 0; i < ota_transport_count; ++i)
    {
        if (strcmp(ota_transports[i]->name, transport->name) == 0)
        {
            return OTA_ERROR_ALREADY_IN_PROGRESS; // Already registered
        }
    }
    ota_transports[ota_transport_count++] = transport;
    return OTA_OK;
}

int ota_manager_unregister_transport(const char *name)
{
    if (!name)
        return OTA_ERROR_INVALID_PARAM;
    for (int i = 0; i < ota_transport_count; ++i)
    {
        if (strcmp(ota_transports[i]->name, name) == 0)
        {
            for (int j = i; j < ota_transport_count - 1; ++j)
            {
                ota_transports[j] = ota_transports[j + 1];
            }
            ota_transports[--ota_transport_count] = NULL;
            return OTA_OK;
        }
    }
    return OTA_ERROR_NOT_INITIALIZED;
}

/* Reduced message queue size */
#define OTA_MSG_QUEUE_SIZE 8

enum ota_msg_type
{
    MSG_START_UPDATE = 1,
    MSG_WRITE_CHUNK,
    MSG_FINALIZE_UPDATE,
    MSG_ABORT_UPDATE,
    MSG_CONFIRM_FIRMWARE,
    MSG_REQUEST_ROLLBACK,
    MSG_REBOOT_REQUEST
};

/* Optimized message structure with union for space efficiency */
struct ota_msg
{
    enum ota_msg_type type : 8;
    uint8_t reserved[3];
    union
    {
        uint32_t expected_size;
        struct
        {
            const uint8_t *data;
            uint16_t length;
        } chunk;
        uint32_t delay_ms;
    } payload;
    struct k_sem *completion_sem;
    enum ota_result *result;
} __packed;

K_MSGQ_DEFINE(ota_msgq, sizeof(struct ota_msg), OTA_MSG_QUEUE_SIZE, 4);

/* Optimized progress reporting */
static inline void update_progress_fast(enum ota_state state)
{
    ota_status.state = state;
    if (ota_status.total_size > 0)
    {
        ota_status.percentage = (uint8_t)((ota_status.bytes_written * 100ULL) / ota_status.total_size);
    }
}

static void update_progress(enum ota_state state, const char *message)
{
    k_mutex_lock(&ota_mutex, K_FOREVER);

    update_progress_fast(state);
    if (message)
    {
        strncpy(ota_status.status_message, message, sizeof(ota_status.status_message) - 1);
        ota_status.status_message[sizeof(ota_status.status_message) - 1] = '\0';
    }

    k_mutex_unlock(&ota_mutex);

    if (progress_callback)
    {
        struct ota_progress progress = {
            .state = ota_status.state,
            .total_size = ota_status.total_size,
            .bytes_written = ota_status.bytes_written,
            .percentage = ota_status.percentage,
            .last_error = ota_status.last_error,
            .status_message = {0}};
        strncpy(progress.status_message, ota_status.status_message, sizeof(progress.status_message) - 1);
        progress_callback(&progress, callback_user_data);
    }

    LOG_INF("OTA: %s (%d%%)", message ? message : "Update", ota_status.percentage);
}

static inline void set_error_fast(enum ota_result error)
{
    ota_status.state = OTA_STATE_ERROR;
    ota_status.last_error = error;
}

static void set_error(enum ota_result error, const char *message)
{
    k_mutex_lock(&ota_mutex, K_FOREVER);
    set_error_fast(error);
    if (message)
    {
        strncpy(ota_status.status_message, message, sizeof(ota_status.status_message) - 1);
        ota_status.status_message[sizeof(ota_status.status_message) - 1] = '\0';
    }
    k_mutex_unlock(&ota_mutex);

    if (progress_callback)
    {
        struct ota_progress progress = {
            .state = ota_status.state,
            .total_size = ota_status.total_size,
            .bytes_written = ota_status.bytes_written,
            .percentage = ota_status.percentage,
            .last_error = ota_status.last_error,
            .status_message = {0}};
        strncpy(progress.status_message, ota_status.status_message, sizeof(progress.status_message) - 1);
        progress_callback(&progress, callback_user_data);
    }

    LOG_ERR("OTA Error: %s (%d)", message ? message : "Unknown", error);
}

/* Flush write buffer to flash */
static enum ota_result flush_write_buffer(void)
{
    if (buffer_pos == 0 || !secondary_fa)
    {
        return OTA_OK;
    }

    /* Get flash write alignment requirement */
    size_t write_alignment = flash_area_align(secondary_fa);

    /* Align buffer size up to write alignment boundary */
    uint16_t aligned_size = ROUND_UP(buffer_pos, write_alignment);

    if (aligned_size > OTA_WRITE_BUFFER_SIZE)
    {
        LOG_ERR("Aligned size too large: %u > %u", aligned_size, OTA_WRITE_BUFFER_SIZE);
        return OTA_ERROR_FLASH_WRITE_FAILED;
    }

    /* Pad with 0xFF (flash erase value) */
    if (aligned_size > buffer_pos)
    {
        memset(&write_buffer[buffer_pos], 0xFF, aligned_size - buffer_pos);
    }

    /* Calculate write offset atomically */
    k_mutex_lock(&ota_mutex, K_FOREVER);
    uint32_t write_offset = ota_status.bytes_written - buffer_pos;
    k_mutex_unlock(&ota_mutex);

    int ret = flash_area_write(secondary_fa, write_offset, write_buffer, aligned_size);
    if (ret)
    {
        LOG_ERR("Flash write failed at offset %u: %d", write_offset, ret);
        return OTA_ERROR_FLASH_WRITE_FAILED;
    }

    buffer_pos = 0;
    return OTA_OK;
}

/* OTA Operations */
static enum ota_result do_start_update(uint32_t expected_size)
{
    LOG_INF("Starting OTA, size: %u", expected_size);

    if (ota_status.state != OTA_STATE_IDLE)
    {
        return OTA_ERROR_ALREADY_IN_PROGRESS;
    }

    /* Open and erase secondary flash */
    int ret = flash_area_open(FLASH_AREA_IMAGE_SECONDARY, &secondary_fa);
    if (ret)
    {
        LOG_ERR("Flash open failed: %d", ret);
        return OTA_ERROR_FLASH_OPEN_FAILED;
    }

    update_progress(OTA_STATE_RECEIVING, "Erasing flash...");
    ret = flash_area_erase(secondary_fa, 0, secondary_fa->fa_size);
    if (ret)
    {
        LOG_ERR("Flash erase failed: %d", ret);
        flash_area_close(secondary_fa);
        secondary_fa = NULL;
        return OTA_ERROR_FLASH_ERASE_FAILED;
    }

    /* Initialize state */
    k_mutex_lock(&ota_mutex, K_FOREVER);
    ota_status.total_size = expected_size > 0 ? expected_size : secondary_fa->fa_size;
    ota_status.bytes_written = 0;
    ota_status.percentage = 0;
    ota_status.last_error = OTA_OK;
    ota_status.last_progress_report = 0;
    k_mutex_unlock(&ota_mutex);

    buffer_pos = 0;

    update_progress(OTA_STATE_RECEIVING, "Ready");
    LOG_INF("OTA started, slot prepared");
    return OTA_OK;
}

static enum ota_result do_write_chunk(const uint8_t *data, uint16_t length)
{
    if (!secondary_fa || !data || length == 0)
    {
        return OTA_ERROR_NOT_INITIALIZED;
    }

    if (ota_status.state != OTA_STATE_RECEIVING)
    {
        LOG_ERR("Write chunk called in wrong state: %d", ota_status.state);
        return OTA_ERROR_INVALID_PARAM;
    }

    /* FIXED: Check bounds before writing */
    if (ota_status.bytes_written + length > secondary_fa->fa_size)
    {
        LOG_ERR("Write would exceed flash size: %u + %u > %u",
                ota_status.bytes_written, length, secondary_fa->fa_size);
        return OTA_ERROR_INSUFFICIENT_SPACE;
    }

    /* FIXED: Check total expected size if set */
    if (ota_status.total_size > 0 &&
        ota_status.bytes_written + length > ota_status.total_size)
    {
        LOG_ERR("Write would exceed expected size: %u + %u > %u",
                ota_status.bytes_written, length, ota_status.total_size);
        return OTA_ERROR_INSUFFICIENT_SPACE;
    }

    enum ota_result result = OTA_OK;
    uint16_t remaining = length;
    const uint8_t *src = data;

    while (remaining > 0 && result == OTA_OK)
    {
        uint16_t copy_size = MIN(remaining, OTA_WRITE_BUFFER_SIZE - buffer_pos);

        /* FIXED: Validate buffer bounds */
        if (buffer_pos + copy_size > OTA_WRITE_BUFFER_SIZE)
        {
            LOG_ERR("Buffer overflow prevented: %u + %u > %u",
                    buffer_pos, copy_size, OTA_WRITE_BUFFER_SIZE);
            return OTA_ERROR_INVALID_PARAM;
        }

        memcpy(&write_buffer[buffer_pos], src, copy_size);
        buffer_pos += copy_size;
        src += copy_size;
        remaining -= copy_size;

        /* Update bytes written counter atomically */
        k_mutex_lock(&ota_mutex, K_FOREVER);
        ota_status.bytes_written += copy_size;
        k_mutex_unlock(&ota_mutex);

        /* Flush buffer when full */
        if (buffer_pos >= OTA_WRITE_BUFFER_SIZE)
        {
            result = flush_write_buffer();
        }

        /* Progress reporting optimization */
        if (ota_status.bytes_written >= ota_status.last_progress_report + OTA_PROGRESS_REPORT_INTERVAL)
        {
            k_mutex_lock(&ota_mutex, K_FOREVER);
            update_progress_fast(OTA_STATE_RECEIVING);
            ota_status.last_progress_report = ota_status.bytes_written;
            k_mutex_unlock(&ota_mutex);

            if (progress_callback)
            {
                struct ota_progress progress = {
                    .state = ota_status.state,
                    .total_size = ota_status.total_size,
                    .bytes_written = ota_status.bytes_written,
                    .percentage = ota_status.percentage,
                    .last_error = ota_status.last_error,
                    .status_message = "Receiving..."};
                progress_callback(&progress, callback_user_data);
            }
        }
    }

    return result;
}

static enum ota_result do_finalize_update(void)
{
    if (!secondary_fa)
    {
        return OTA_ERROR_NOT_INITIALIZED;
    }

    if (ota_status.state != OTA_STATE_RECEIVING)
    {
        return OTA_ERROR_INVALID_PARAM;
    }

    /* Flush remaining data */
    enum ota_result result = flush_write_buffer();
    if (result != OTA_OK)
    {
        return result;
    }

    update_progress(OTA_STATE_VALIDATING, "Validating...");

    /* Quick validation */
    if (ota_status.bytes_written == 0)
    {
        set_error(OTA_ERROR_INVALID_IMAGE, "No data");
        return OTA_ERROR_INVALID_IMAGE;
    }

    /* Check image header */
    uint32_t magic;
    int ret = flash_area_read(secondary_fa, 0, &magic, sizeof(magic));
    if (ret || magic != 0x96f3b83d)
    {
        set_error(OTA_ERROR_INVALID_IMAGE, "Invalid format");
        return OTA_ERROR_INVALID_IMAGE;
    }

    update_progress(OTA_STATE_INSTALLING, "Installing...");

    /* Request boot upgrade */
    ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
    if (ret)
    {
        set_error(OTA_ERROR_BOOT_REQUEST_FAILED, "Boot request failed");
        return OTA_ERROR_BOOT_REQUEST_FAILED;
    }

    flash_area_close(secondary_fa);
    secondary_fa = NULL;

    update_progress(OTA_STATE_COMPLETE, "Ready - reboot to apply");
    LOG_INF("OTA complete");
    return OTA_OK;
}

static enum ota_result do_abort_update(void)
{
    if (secondary_fa)
    {
        flush_write_buffer(); // Ignore errors during abort
        flash_area_close(secondary_fa);
        secondary_fa = NULL;
    }

    k_mutex_lock(&ota_mutex, K_FOREVER);
    ota_status.state = OTA_STATE_IDLE;
    ota_status.total_size = 0;
    ota_status.bytes_written = 0;
    ota_status.percentage = 0;
    ota_status.last_error = OTA_OK;
    strcpy(ota_status.status_message, "Aborted");
    k_mutex_unlock(&ota_mutex);

    buffer_pos = 0;
    LOG_INF("OTA aborted");
    return OTA_OK;
}

static enum ota_result do_confirm_firmware(void)
{
    int ret = boot_write_img_confirmed();
    return (ret == 0) ? OTA_OK : OTA_ERROR_BOOT_REQUEST_FAILED;
}

/* Optimized thread main function */
static void ota_thread_main(void *p1, void *p2, void *p3)
{
    struct ota_msg msg;
    enum ota_result result;

    LOG_INF("OTA thread ready");

    while (1)
    {
        printk("OTA: Waiting for OTA message...\n");
        if (k_msgq_get(&ota_msgq, &msg, K_FOREVER) == 0)
        {
            printk("OTA: OTA message received.\n");
            result = OTA_ERROR_INVALID_PARAM;

            switch (msg.type)
            {
            case MSG_START_UPDATE:
                result = do_start_update(msg.payload.expected_size);
                break;
            case MSG_WRITE_CHUNK:
                result = do_write_chunk(msg.payload.chunk.data, msg.payload.chunk.length);
                break;
            case MSG_FINALIZE_UPDATE:
                result = do_finalize_update();
                break;
            case MSG_ABORT_UPDATE:
                result = do_abort_update();
                break;
            case MSG_CONFIRM_FIRMWARE:
                result = do_confirm_firmware();
                break;
            case MSG_REQUEST_ROLLBACK:
                result = OTA_OK;
                k_sleep(K_MSEC(1000));
                sys_reboot(SYS_REBOOT_WARM);
                break;
            case MSG_REBOOT_REQUEST:
                if (msg.payload.delay_ms > 0)
                {
                    k_sleep(K_MSEC(msg.payload.delay_ms));
                }
                sys_reboot(SYS_REBOOT_WARM);
                break;
            }

            /* Signal completion */
            if (msg.result)
            {
                *msg.result = result;
            }
            if (msg.completion_sem)
            {
                k_sem_give(msg.completion_sem);
            }

            /* Handle errors for non-write operations */
            if (result != OTA_OK && msg.type != MSG_WRITE_CHUNK)
            {
                set_error(result, ota_result_to_string(result));
            }
        }
    }
}

/* Optimized message sending */
static enum ota_result send_ota_message_sync(struct ota_msg *msg)
{
    enum ota_result result = OTA_ERROR_TIMEOUT;
    struct k_sem completion_sem;

    k_sem_init(&completion_sem, 0, 1);
    msg->completion_sem = &completion_sem;
    msg->result = &result;

    if (k_msgq_put(&ota_msgq, msg, K_MSEC(1000)) != 0)
    {
        LOG_ERR("Failed to send OTA message (queue full)");
        return OTA_ERROR_TIMEOUT;
    }

    /* Wait with timeout to prevent infinite blocking */
    if (k_sem_take(&completion_sem, K_MSEC(30000)) != 0)
    {
        LOG_ERR("OTA operation timed out");
        return OTA_ERROR_TIMEOUT;
    }

    return result;
}

/* Public API */
int ota_manager_init(void)
{
    memset(&ota_status, 0, sizeof(ota_status));
    ota_status.state = OTA_STATE_IDLE;
    strcpy(ota_status.status_message, "Initialized");

    ota_thread_id = k_thread_create(&ota_thread_data,
                                    ota_thread_stack,
                                    K_THREAD_STACK_SIZEOF(ota_thread_stack),
                                    ota_thread_main,
                                    NULL, NULL, NULL,
                                    OTA_THREAD_PRIORITY,
                                    0, K_NO_WAIT);

    k_thread_name_set(ota_thread_id, "ota_mgr");
    LOG_INF("OTA Manager ready");
    return 0;
}

enum ota_result ota_start_update(size_t expected_size)
{
    struct ota_msg msg = {
        .type = MSG_START_UPDATE,
        .payload.expected_size = expected_size};
    return send_ota_message_sync(&msg);
}

enum ota_result ota_write_chunk(const uint8_t *data, size_t length)
{
    if (!data || length == 0 || length > 65535)
    {
        return OTA_ERROR_INVALID_PARAM;
    }

    /* For write operations, we need synchronous handling to detect errors */
    struct ota_msg msg = {
        .type = MSG_WRITE_CHUNK,
        .payload.chunk = {.data = data, .length = (uint16_t)length}};

    return send_ota_message_sync(&msg);
}

enum ota_result ota_finalize_update(void)
{
    struct ota_msg msg = {.type = MSG_FINALIZE_UPDATE};
    return send_ota_message_sync(&msg);
}

enum ota_result ota_abort_update(void)
{
    struct ota_msg msg = {.type = MSG_ABORT_UPDATE};
    return send_ota_message_sync(&msg);
}

const struct ota_progress *ota_get_progress(void)
{
    static struct ota_progress progress;

    k_mutex_lock(&ota_mutex, K_FOREVER);
    progress.state = ota_status.state;
    progress.total_size = ota_status.total_size;
    progress.bytes_written = ota_status.bytes_written;
    progress.percentage = ota_status.percentage;
    progress.last_error = ota_status.last_error;
    strncpy(progress.status_message, ota_status.status_message,
            sizeof(progress.status_message) - 1);
    k_mutex_unlock(&ota_mutex);

    return &progress;
}

enum ota_result ota_confirm_firmware(void)
{
    struct ota_msg msg = {.type = MSG_CONFIRM_FIRMWARE};
    return send_ota_message_sync(&msg);
}

enum ota_result ota_register_progress_callback(ota_progress_cb_t callback, void *user_data)
{
    progress_callback = callback;
    callback_user_data = user_data;
    return OTA_OK;
}

bool ota_is_update_in_progress(void)
{
    return (ota_status.state == OTA_STATE_RECEIVING ||
            ota_status.state == OTA_STATE_VALIDATING ||
            ota_status.state == OTA_STATE_INSTALLING);
}

/* String conversion functions */
const char *ota_result_to_string(enum ota_result result)
{
    static const char *const strings[] = {
        "OK",
        "Invalid param",
        "Not initialized",
        "In progress",
        "Flash open failed",
        "Flash erase failed",
        "Flash write failed",
        "Invalid image",
        "Insufficient space",
        "Timeout",
        "Boot request failed"};

    return (result < ARRAY_SIZE(strings) && strings[result]) ? strings[result] : "Unknown error";
}

const char *ota_state_to_string(enum ota_state state)
{
    static const char *const strings[] = {
        [OTA_STATE_IDLE] = "Idle",
        [OTA_STATE_RECEIVING] = "Receiving",
        [OTA_STATE_VALIDATING] = "Validating",
        [OTA_STATE_INSTALLING] = "Installing",
        [OTA_STATE_COMPLETE] = "Complete",
        [OTA_STATE_ERROR] = "Error"};

    return (state < ARRAY_SIZE(strings) && strings[state]) ? strings[state] : "Unknown";
}

void ota_reboot_to_apply_update(uint32_t delay_ms)
{
    struct ota_msg msg = {
        .type = MSG_REBOOT_REQUEST,
        .payload.delay_ms = delay_ms,
        .completion_sem = NULL,
        .result = NULL};
    k_msgq_put(&ota_msgq, &msg, K_NO_WAIT);
}

/* Simplified shell commands */
static int cmd_ota_status(const struct shell *sh, size_t argc, char **argv)
{
    const struct ota_progress *p = ota_get_progress();
    shell_print(sh, "State: %s (%d%%) - %s",
                ota_state_to_string(p->state), p->percentage, p->status_message);
    return 0;
}

static int cmd_ota_confirm(const struct shell *sh, size_t argc, char **argv)
{
    enum ota_result result = ota_confirm_firmware();
    shell_print(sh, result == OTA_OK ? "Confirmed" : "Failed: %s",
                ota_result_to_string(result));
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ota_cmds,
                               SHELL_CMD(status, NULL, "OTA status", cmd_ota_status),
                               SHELL_CMD(confirm, NULL, "Confirm firmware", cmd_ota_confirm),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ota, &ota_cmds, "OTA management", NULL);