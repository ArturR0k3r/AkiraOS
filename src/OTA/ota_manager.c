/**
 * @file ota_manager.c
 * @brief OTA Manager Implementation for ESP32
 *
 * Thread-based OTA management with MCUboot integration
 */

#include "ota_manager.h"
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(ota_manager, LOG_LEVEL_INF);

#define FLASH_AREA_IMAGE_PRIMARY FIXED_PARTITION_ID(slot0_partition)
#define FLASH_AREA_IMAGE_SECONDARY FIXED_PARTITION_ID(slot1_partition)

/* Thread stack and control block */
static K_THREAD_STACK_DEFINE(ota_thread_stack, OTA_THREAD_STACK_SIZE);
static struct k_thread ota_thread_data;
static k_tid_t ota_thread_id;

/* OTA state management */
static struct ota_progress current_progress = {
    .state = OTA_STATE_IDLE,
    .total_size = 0,
    .bytes_written = 0,
    .percentage = 0,
    .last_error = OTA_OK,
    .status_message = "Ready"};

static K_MUTEX_DEFINE(ota_mutex);

/* Flash area handle for secondary slot */
static const struct flash_area *secondary_fa = NULL;

/* Progress callback */
static ota_progress_cb_t progress_callback = NULL;
static void *callback_user_data = NULL;

/* OTA operation messages */
#define OTA_MSG_QUEUE_SIZE 20

enum ota_msg_type
{
    MSG_START_UPDATE,
    MSG_WRITE_CHUNK,
    MSG_FINALIZE_UPDATE,
    MSG_ABORT_UPDATE,
    MSG_CONFIRM_FIRMWARE,
    MSG_REQUEST_ROLLBACK,
    MSG_REBOOT_REQUEST
};

struct ota_chunk_data
{
    uint8_t data[OTA_MAX_CHUNK_SIZE];
    size_t length;
};

struct ota_msg
{
    enum ota_msg_type type;
    union
    {
        struct
        {
            size_t expected_size;
        } start_update;
        struct ota_chunk_data chunk;
        struct
        {
            uint32_t delay_ms;
        } reboot;
    } data;
    struct k_sem *completion_sem;
    enum ota_result *result;
};

K_MSGQ_DEFINE(ota_msgq, sizeof(struct ota_msg), OTA_MSG_QUEUE_SIZE, 4);

/* Helper functions */
static void update_progress(enum ota_state state, const char *message)
{
    k_mutex_lock(&ota_mutex, K_FOREVER);

    current_progress.state = state;
    if (message)
    {
        strncpy(current_progress.status_message, message, sizeof(current_progress.status_message) - 1);
        current_progress.status_message[sizeof(current_progress.status_message) - 1] = '\0';
    }

    if (current_progress.total_size > 0)
    {
        current_progress.percentage = (current_progress.bytes_written * 100) / current_progress.total_size;
    }

    k_mutex_unlock(&ota_mutex);

    if (progress_callback)
    {
        progress_callback(&current_progress, callback_user_data);
    }

    LOG_INF("OTA Progress: %s (%d%%)", message ? message : "Update", current_progress.percentage);
}

static void set_error(enum ota_result error, const char *message)
{
    k_mutex_lock(&ota_mutex, K_FOREVER);
    current_progress.state = OTA_STATE_ERROR;
    current_progress.last_error = error;
    if (message)
    {
        strncpy(current_progress.status_message, message, sizeof(current_progress.status_message) - 1);
        current_progress.status_message[sizeof(current_progress.status_message) - 1] = '\0';
    }
    k_mutex_unlock(&ota_mutex);

    if (progress_callback)
    {
        progress_callback(&current_progress, callback_user_data);
    }

    LOG_ERR("OTA Error: %s (code: %d)", message ? message : "Unknown error", error);
}

/* OTA Operations - executed in OTA thread */
static enum ota_result do_start_update(size_t expected_size)
{
    LOG_INF("Starting OTA update, expected size: %zu bytes", expected_size);

    if (current_progress.state != OTA_STATE_IDLE)
    {
        return OTA_ERROR_ALREADY_IN_PROGRESS;
    }

    /* Open secondary flash area */
    int ret = flash_area_open(FLASH_AREA_IMAGE_SECONDARY, &secondary_fa);
    if (ret)
    {
        LOG_ERR("Failed to open secondary flash area: %d", ret);
        return OTA_ERROR_FLASH_OPEN_FAILED;
    }

    /* Erase secondary slot */
    update_progress(OTA_STATE_RECEIVING, "Erasing flash...");
    ret = flash_area_erase(secondary_fa, 0, secondary_fa->fa_size);
    if (ret)
    {
        LOG_ERR("Failed to erase secondary flash area: %d", ret);
        flash_area_close(secondary_fa);
        secondary_fa = NULL;
        return OTA_ERROR_FLASH_ERASE_FAILED;
    }

    /* Initialize progress tracking */
    k_mutex_lock(&ota_mutex, K_FOREVER);
    current_progress.total_size = expected_size > 0 ? expected_size : secondary_fa->fa_size;
    current_progress.bytes_written = 0;
    current_progress.percentage = 0;
    current_progress.last_error = OTA_OK;
    k_mutex_unlock(&ota_mutex);

    update_progress(OTA_STATE_RECEIVING, "Ready to receive firmware");

    LOG_INF("OTA update started, secondary slot prepared");
    return OTA_OK;
}

static enum ota_result do_write_chunk(const uint8_t *data, size_t length)
{
    if (!secondary_fa)
    {
        return OTA_ERROR_NOT_INITIALIZED;
    }

    if (current_progress.state != OTA_STATE_RECEIVING)
    {
        return OTA_ERROR_INVALID_PARAM;
    }

    if (current_progress.bytes_written + length > secondary_fa->fa_size)
    {
        LOG_ERR("Firmware too large: %zu + %zu > %zu",
                current_progress.bytes_written, length, secondary_fa->fa_size);
        return OTA_ERROR_INSUFFICIENT_SPACE;
    }

    /* Write chunk to flash */
    int ret = flash_area_write(secondary_fa, current_progress.bytes_written, data, length);
    if (ret)
    {
        LOG_ERR("Flash write failed at offset %zu: %d", current_progress.bytes_written, ret);
        return OTA_ERROR_FLASH_WRITE_FAILED;
    }

    /* Update progress */
    k_mutex_lock(&ota_mutex, K_FOREVER);
    current_progress.bytes_written += length;
    k_mutex_unlock(&ota_mutex);

    /* Report progress periodically */
    if (current_progress.bytes_written % OTA_PROGRESS_REPORT_SIZE == 0 ||
        current_progress.bytes_written >= current_progress.total_size)
    {

        char msg[64];
        snprintf(msg, sizeof(msg), "Received %zu/%zu bytes",
                 current_progress.bytes_written, current_progress.total_size);
        update_progress(OTA_STATE_RECEIVING, msg);
    }

    LOG_DBG("Written %zu bytes, total: %zu", length, current_progress.bytes_written);
    return OTA_OK;
}

static enum ota_result do_finalize_update(void)
{
    if (!secondary_fa)
    {
        return OTA_ERROR_NOT_INITIALIZED;
    }

    if (current_progress.state != OTA_STATE_RECEIVING)
    {
        return OTA_ERROR_INVALID_PARAM;
    }

    update_progress(OTA_STATE_VALIDATING, "Validating firmware...");

    /* Basic validation - check if we have data */
    if (current_progress.bytes_written == 0)
    {
        set_error(OTA_ERROR_INVALID_IMAGE, "No firmware data received");
        return OTA_ERROR_INVALID_IMAGE;
    }

    /* Read and validate image header */
    struct ota_image_info img_info;
    int ret = flash_area_read(secondary_fa, 0, &img_info, sizeof(img_info));
    if (ret)
    {
        set_error(OTA_ERROR_INVALID_IMAGE, "Failed to read image header");
        return OTA_ERROR_INVALID_IMAGE;
    }

    /* Check image magic number (MCUboot image magic) */
    if (img_info.magic != 0x96f3b83d)
    {
        LOG_ERR("Invalid image magic: 0x%08x", img_info.magic);
        set_error(OTA_ERROR_INVALID_IMAGE, "Invalid image format");
        return OTA_ERROR_INVALID_IMAGE;
    }

    update_progress(OTA_STATE_INSTALLING, "Installing firmware...");

    /* Request MCUboot to swap slots on next boot */
    ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
    if (ret)
    {
        LOG_ERR("Boot upgrade request failed: %d", ret);
        set_error(OTA_ERROR_BOOT_REQUEST_FAILED, "Failed to schedule firmware update");
        return OTA_ERROR_BOOT_REQUEST_FAILED;
    }

    /* Close flash area */
    flash_area_close(secondary_fa);
    secondary_fa = NULL;

    update_progress(OTA_STATE_COMPLETE, "Firmware update ready - reboot to apply");

    LOG_INF("OTA update finalized, reboot required to apply");
    return OTA_OK;
}

static enum ota_result do_abort_update(void)
{
    if (secondary_fa)
    {
        flash_area_close(secondary_fa);
        secondary_fa = NULL;
    }

    k_mutex_lock(&ota_mutex, K_FOREVER);
    current_progress.state = OTA_STATE_IDLE;
    current_progress.total_size = 0;
    current_progress.bytes_written = 0;
    current_progress.percentage = 0;
    current_progress.last_error = OTA_OK;
    strcpy(current_progress.status_message, "Update aborted");
    k_mutex_unlock(&ota_mutex);

    LOG_INF("OTA update aborted");
    return OTA_OK;
}

static enum ota_result do_confirm_firmware(void)
{
    int ret = boot_write_img_confirmed();
    if (ret)
    {
        LOG_ERR("Failed to confirm image: %d", ret);
        return OTA_ERROR_BOOT_REQUEST_FAILED;
    }

    LOG_INF("Current firmware confirmed as permanent");
    return OTA_OK;
}

static enum ota_result do_request_rollback(void)
{
    /* MCUboot will automatically rollback on next reboot if image is not confirmed */
    LOG_INF("Rollback requested - rebooting to previous firmware");
    return OTA_OK;
}

static void do_reboot_request(uint32_t delay_ms)
{
    LOG_INF("Rebooting system in %u ms to apply firmware update", delay_ms);

    if (delay_ms > 0)
    {
        k_sleep(K_MSEC(delay_ms));
    }

    sys_reboot(SYS_REBOOT_WARM);
}

/* OTA thread main function */
static void ota_thread_main(void *p1, void *p2, void *p3)
{
    struct ota_msg msg;

    LOG_INF("OTA manager thread started");

    while (1)
    {
        if (k_msgq_get(&ota_msgq, &msg, K_FOREVER) == 0)
        {
            enum ota_result result = OTA_ERROR_INVALID_PARAM;

            switch (msg.type)
            {
            case MSG_START_UPDATE:
                result = do_start_update(msg.data.start_update.expected_size);
                break;

            case MSG_WRITE_CHUNK:
                result = do_write_chunk(msg.data.chunk.data, msg.data.chunk.length);
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
                result = do_request_rollback();
                /* After rollback request, reboot */
                do_reboot_request(1000);
                break;

            case MSG_REBOOT_REQUEST:
                do_reboot_request(msg.data.reboot.delay_ms);
                break;
            }

            /* Signal completion if requested */
            if (msg.result)
            {
                *msg.result = result;
            }
            if (msg.completion_sem)
            {
                k_sem_give(msg.completion_sem);
            }

            /* Handle errors */
            if (result != OTA_OK && msg.type != MSG_WRITE_CHUNK)
            {
                set_error(result, ota_result_to_string(result));
            }
        }
    }
}

/* Helper function to send message and wait for completion */
static enum ota_result send_ota_message(struct ota_msg *msg)
{
    enum ota_result result;
    struct k_sem completion_sem;

    k_sem_init(&completion_sem, 0, 1);
    msg->completion_sem = &completion_sem;
    msg->result = &result;

    if (k_msgq_put(&ota_msgq, msg, K_NO_WAIT) != 0)
    {
        LOG_ERR("OTA message queue full");
        return OTA_ERROR_TIMEOUT;
    }

    k_sem_take(&completion_sem, K_FOREVER);
    return result;
}

/* Public API implementation */
int ota_manager_init(void)
{
    /* Initialize progress state */
    k_mutex_lock(&ota_mutex, K_FOREVER);
    current_progress.state = OTA_STATE_IDLE;
    strcpy(current_progress.status_message, "OTA Manager initialized");
    k_mutex_unlock(&ota_mutex);

    /* Start OTA thread */
    ota_thread_id = k_thread_create(&ota_thread_data,
                                    ota_thread_stack,
                                    K_THREAD_STACK_SIZEOF(ota_thread_stack),
                                    ota_thread_main,
                                    NULL, NULL, NULL,
                                    OTA_THREAD_PRIORITY,
                                    0, K_NO_WAIT);

    k_thread_name_set(ota_thread_id, "ota_manager");

    LOG_INF("OTA Manager initialized");
    return 0;
}

enum ota_result ota_start_update(size_t expected_size)
{
    struct ota_msg msg = {
        .type = MSG_START_UPDATE,
        .data.start_update.expected_size = expected_size};

    return send_ota_message(&msg);
}

enum ota_result ota_write_chunk(const uint8_t *data, size_t length)
{
    if (!data || length == 0 || length > OTA_MAX_CHUNK_SIZE)
    {
        return OTA_ERROR_INVALID_PARAM;
    }

    struct ota_msg msg = {
        .type = MSG_WRITE_CHUNK,
        .data.chunk.length = length};

    memcpy(msg.data.chunk.data, data, length);

    /* For write chunks, send without waiting to improve performance */
    if (k_msgq_put(&ota_msgq, &msg, K_NO_WAIT) != 0)
    {
        LOG_ERR("OTA message queue full");
        return OTA_ERROR_TIMEOUT;
    }

    return OTA_OK;
}

enum ota_result ota_finalize_update(void)
{
    struct ota_msg msg = {
        .type = MSG_FINALIZE_UPDATE};

    return send_ota_message(&msg);
}

enum ota_result ota_abort_update(void)
{
    struct ota_msg msg = {
        .type = MSG_ABORT_UPDATE};

    return send_ota_message(&msg);
}

const struct ota_progress *ota_get_progress(void)
{
    return &current_progress;
}

int ota_get_status(bool *is_confirmed, bool *is_pending_revert)
{
    if (!is_confirmed || !is_pending_revert)
    {
        return -EINVAL;
    }

    *is_confirmed = boot_is_img_confirmed();
    *is_pending_revert = !(*is_confirmed); // If not confirmed, revert is pending

    return 0;
}

enum ota_result ota_confirm_firmware(void)
{
    struct ota_msg msg = {
        .type = MSG_CONFIRM_FIRMWARE};

    return send_ota_message(&msg);
}

enum ota_result ota_request_rollback(void)
{
    struct ota_msg msg = {
        .type = MSG_REQUEST_ROLLBACK};

    return send_ota_message(&msg);
}

int ota_get_image_info(int slot, struct ota_image_info *info)
{
    if (!info || (slot != 0 && slot != 1))
    {
        return -EINVAL;
    }

    const struct flash_area *fa;
    uint8_t flash_area_id = (slot == 0) ? FLASH_AREA_IMAGE_PRIMARY : FLASH_AREA_IMAGE_SECONDARY;

    int ret = flash_area_open(flash_area_id, &fa);
    if (ret)
    {
        return ret;
    }

    ret = flash_area_read(fa, 0, info, sizeof(*info));
    flash_area_close(fa);

    return ret;
}

enum ota_result ota_register_progress_callback(ota_progress_cb_t callback, void *user_data)
{
    progress_callback = callback;
    callback_user_data = user_data;
    return OTA_OK;
}

bool ota_is_update_in_progress(void)
{
    return (current_progress.state == OTA_STATE_RECEIVING ||
            current_progress.state == OTA_STATE_VALIDATING ||
            current_progress.state == OTA_STATE_INSTALLING);
}

int ota_get_slot_sizes(size_t *primary_size, size_t *secondary_size)
{
    if (!primary_size || !secondary_size)
    {
        return -EINVAL;
    }

    const struct flash_area *fa;
    int ret;

    /* Get primary slot size */
    ret = flash_area_open(FLASH_AREA_IMAGE_PRIMARY, &fa);
    if (ret)
    {
        return ret;
    }
    *primary_size = fa->fa_size;
    flash_area_close(fa);

    /* Get secondary slot size */
    ret = flash_area_open(FLASH_AREA_IMAGE_SECONDARY, &fa);
    if (ret)
    {
        return ret;
    }
    *secondary_size = fa->fa_size;
    flash_area_close(fa);

    return 0;
}

const char *ota_result_to_string(enum ota_result result)
{
    switch (result)
    {
    case OTA_OK:
        return "Success";
    case OTA_ERROR_INVALID_PARAM:
        return "Invalid parameter";
    case OTA_ERROR_NOT_INITIALIZED:
        return "Not initialized";
    case OTA_ERROR_ALREADY_IN_PROGRESS:
        return "Update already in progress";
    case OTA_ERROR_FLASH_OPEN_FAILED:
        return "Flash open failed";
    case OTA_ERROR_FLASH_ERASE_FAILED:
        return "Flash erase failed";
    case OTA_ERROR_FLASH_WRITE_FAILED:
        return "Flash write failed";
    case OTA_ERROR_INVALID_IMAGE:
        return "Invalid image format";
    case OTA_ERROR_SIGNATURE_VERIFICATION:
        return "Signature verification failed";
    case OTA_ERROR_INSUFFICIENT_SPACE:
        return "Insufficient flash space";
    case OTA_ERROR_TIMEOUT:
        return "Operation timeout";
    case OTA_ERROR_BOOT_REQUEST_FAILED:
        return "Boot request failed";
    default:
        return "Unknown error";
    }
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
    struct ota_msg msg = {
        .type = MSG_REBOOT_REQUEST,
        .data.reboot.delay_ms = delay_ms,
        .completion_sem = NULL,
        .result = NULL};

    k_msgq_put(&ota_msgq, &msg, K_NO_WAIT);
}

/* Shell Commands */
static int cmd_ota_status(const struct shell *sh, size_t argc, char **argv)
{
    const struct ota_progress *progress = ota_get_progress();
    bool is_confirmed, is_pending_revert;

    shell_print(sh, "\n=== OTA Status ===");
    shell_print(sh, "State: %s", ota_state_to_string(progress->state));
    shell_print(sh, "Progress: %d%% (%zu/%zu bytes)",
                progress->percentage, progress->bytes_written, progress->total_size);
    shell_print(sh, "Status: %s", progress->status_message);

    if (progress->last_error != OTA_OK)
    {
        shell_print(sh, "Last Error: %s", ota_result_to_string(progress->last_error));
    }

    if (ota_get_status(&is_confirmed, &is_pending_revert) == 0)
    {
        shell_print(sh, "Current Firmware: %s", is_confirmed ? "Confirmed" : "Test (pending confirmation)");
        if (is_pending_revert)
        {
            shell_print(sh, "WARNING: Firmware will revert on next reboot unless confirmed");
        }
    }

    size_t primary_size, secondary_size;
    if (ota_get_slot_sizes(&primary_size, &secondary_size) == 0)
    {
        shell_print(sh, "Flash Slots: Primary=%zu bytes, Secondary=%zu bytes",
                    primary_size, secondary_size);
    }

    return 0;
}

static int cmd_ota_confirm(const struct shell *sh, size_t argc, char **argv)
{
    enum ota_result result = ota_confirm_firmware();
    if (result == OTA_OK)
    {
        shell_print(sh, "Current firmware confirmed as permanent");
    }
    else
    {
        shell_error(sh, "Failed to confirm firmware: %s", ota_result_to_string(result));
    }
    return 0;
}

static int cmd_ota_rollback(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "WARNING: This will revert to the previous firmware version!");
    shell_print(sh, "System will reboot automatically.");
    shell_print(sh, "Type 'ota rollback confirm' to proceed.");

    if (argc > 1 && strcmp(argv[1], "confirm") == 0)
    {
        enum ota_result result = ota_request_rollback();
        if (result == OTA_OK)
        {
            shell_print(sh, "Rollback initiated - rebooting...");
        }
        else
        {
            shell_error(sh, "Failed to request rollback: %s", ota_result_to_string(result));
        }
    }

    return 0;
}

static int cmd_ota_abort(const struct shell *sh, size_t argc, char **argv)
{
    if (!ota_is_update_in_progress())
    {
        shell_print(sh, "No OTA update in progress");
        return 0;
    }

    enum ota_result result = ota_abort_update();
    if (result == OTA_OK)
    {
        shell_print(sh, "OTA update aborted");
    }
    else
    {
        shell_error(sh, "Failed to abort update: %s", ota_result_to_string(result));
    }

    return 0;
}

static int cmd_ota_image_info(const struct shell *sh, size_t argc, char **argv)
{
    int slot = 0;

    if (argc > 1)
    {
        slot = atoi(argv[1]);
        if (slot != 0 && slot != 1)
        {
            shell_error(sh, "Invalid slot number. Use 0 (primary) or 1 (secondary)");
            return -EINVAL;
        }
    }

    struct ota_image_info info;
    int ret = ota_get_image_info(slot, &info);
    if (ret != 0)
    {
        shell_error(sh, "Failed to read image info from slot %d: %d", slot, ret);
        return ret;
    }

    shell_print(sh, "\n=== Image Info (Slot %d) ===", slot);
    shell_print(sh, "Magic: 0x%08x %s", info.magic,
                info.magic == 0x96f3b83d ? "(Valid MCUboot)" : "(Invalid)");
    shell_print(sh, "Load Address: 0x%08x", info.load_addr);
    shell_print(sh, "Header Size: %d bytes", info.hdr_size);
    shell_print(sh, "Image Size: %d bytes", info.img_size);
    shell_print(sh, "Version: %d.%d.%d+%d",
                info.version.major, info.version.minor,
                info.version.revision, info.version.build_num);
    shell_print(sh, "Flags: 0x%08x", info.flags);

    return 0;
}

static int cmd_ota_reboot(const struct shell *sh, size_t argc, char **argv)
{
    uint32_t delay_ms = 3000; // Default 3 second delay

    if (argc > 1)
    {
        delay_ms = strtoul(argv[1], NULL, 10);
        if (delay_ms > 60000)
        { // Max 60 seconds
            delay_ms = 60000;
        }
    }

    shell_print(sh, "System will reboot in %u milliseconds...", delay_ms);
    ota_reboot_to_apply_update(delay_ms);

    return 0;
}

/* Shell command registration */
SHELL_STATIC_SUBCMD_SET_CREATE(ota_cmds,
                               SHELL_CMD(status, NULL, "Show OTA status and firmware info", cmd_ota_status),
                               SHELL_CMD(confirm, NULL, "Confirm current firmware as permanent", cmd_ota_confirm),
                               SHELL_CMD(rollback, NULL, "Rollback to previous firmware", cmd_ota_rollback),
                               SHELL_CMD(abort, NULL, "Abort ongoing OTA update", cmd_ota_abort),
                               SHELL_CMD(image_info, NULL, "Show image info for slot (0=primary, 1=secondary)", cmd_ota_image_info),
                               SHELL_CMD(reboot, NULL, "Reboot system [delay_ms]", cmd_ota_reboot),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ota, &ota_cmds, "OTA firmware management", NULL);