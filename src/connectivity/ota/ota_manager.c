/**
 * @file ota_manager.c
 * @brief OTA Manager Implementation
 * Optimized OTA manager for ESP32 device with MCUboot integration
 * Handles firmware updates, validation, and rollback functionality
 * with reduced memory footprint and improved performance.
 */

#include "ota_manager.h"
#include "../transport_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "akira.h"

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

/* Optimized buffer sizes - 4KB aligned for flash page writes */
#define OTA_WRITE_BUFFER_SIZE 4096        // Align with flash page size
#define OTA_PROGRESS_REPORT_INTERVAL 8192 // Report every 8KB

/*===========================================================================*/
/* OTA Worker Thread Infrastructure                                          */
/*===========================================================================*/

/** Command types posted to the OTA worker thread */
enum ota_cmd_type
{
    OTA_CMD_START = 1, /**< Begin update: erase flash, then receive data */
    OTA_CMD_FINALIZE,  /**< Drain pipe, validate, mark for boot */
    OTA_CMD_ABORT,     /**< Drain and discard, restore IDLE state */
};

struct ota_cmd
{
    enum ota_cmd_type type;
    uint32_t size; /**< Expected firmware size (CMD_START only) */
};

/** Command queue: transport threads post commands, worker consumes */
K_MSGQ_DEFINE(ota_cmd_q, sizeof(struct ota_cmd), 4, 4);

/**
 * Data pipe: transport threads push raw firmware bytes, worker writes to flash.
 * Sized to absorb ~16 x 512-byte HTTP chunks while flash erase is underway.
 * Back-pressure: k_pipe_put blocks the transport thread when pipe is full,
 * preventing TCP/BLE/USB timeouts by keeping connections alive.
 */
#define OTA_DATA_PIPE_SIZE 8192
K_PIPE_DEFINE(ota_data_pipe, OTA_DATA_PIPE_SIZE, 4);

/** Signaled by worker when CMD_FINALIZE or CMD_ABORT finishes */
K_SEM_DEFINE(ota_done_sem, 0, 1);

/** Result written by worker, read by caller of ota_finalize_update() */
static enum ota_result ota_thread_result = OTA_OK;

/** OTA worker thread objects */
K_THREAD_STACK_DEFINE(ota_worker_stack, OTA_THREAD_STACK_SIZE);
static struct k_thread ota_worker_data;
static k_tid_t ota_worker_tid;

/** Transport handler ID for firmware data type registration */
static int ota_transport_handler_id = -1;

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

/* Forward declaration for transport callback */
static int ota_data_callback(const uint8_t *data, size_t len,
                             const struct transport_chunk_info *info);

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

/* Forward declaration */
static enum ota_result do_abort_update(void);

/* OTA Operations */
static enum ota_result do_start_update(uint32_t expected_size)
{
    // LOG_INF("=== do_start_update called ===");
    LOG_INF("Starting OTA, size: %u bytes (%.1f KB)", expected_size, expected_size / 1024.0);

    /* If already in progress, abort the previous one first */
    if (ota_status.state != OTA_STATE_IDLE)
    {
        LOG_WRN("OTA already in state %d, aborting previous...", ota_status.state);
        do_abort_update();
    }

    /* Ensure we start fresh */
    if (secondary_fa != NULL)
    {
        flash_area_close(secondary_fa);
        secondary_fa = NULL;
    }
    buffer_pos = 0;

    /* Open secondary flash */
    int ret = flash_area_open(FLASH_AREA_IMAGE_SECONDARY, &secondary_fa);
    if (ret)
    {
        LOG_ERR("Flash open failed: %d", ret);
        update_progress(OTA_STATE_ERROR, "Flash open failed");
        return OTA_ERROR_FLASH_OPEN_FAILED;
    }

    /* Log partition boundaries for verification */
    LOG_INF("OTA Secondary slot: offset=0x%08" PRIx32 " size=0x%08" PRIx32 " (%u KB)",
            (uint32_t)secondary_fa->fa_off,
            (uint32_t)secondary_fa->fa_size,
            (uint32_t)(secondary_fa->fa_size / 1024));
    LOG_INF("Note: Storage partition (LittleFS) is separate and NOT erased");

    update_progress(OTA_STATE_RECEIVING, "Erasing flash...");
    LOG_INF("OTA: Erasing flash... (0%%)");

    ret = flash_area_erase(secondary_fa, 0, secondary_fa->fa_size);
    if (ret)
    {
        LOG_ERR("Flash erase failed: %d", ret);
        flash_area_close(secondary_fa);
        secondary_fa = NULL;
        update_progress(OTA_STATE_ERROR, "Flash erase failed");
        return OTA_ERROR_FLASH_ERASE_FAILED;
    }

    LOG_INF("OTA: Ready (0%%)");

    /* Initialize state */
    k_mutex_lock(&ota_mutex, K_FOREVER);
    ota_status.total_size = expected_size > 0 ? expected_size : secondary_fa->fa_size;
    ota_status.bytes_written = 0;
    ota_status.percentage = 0;
    ota_status.last_error = OTA_OK;
    ota_status.last_progress_report = 0;
    k_mutex_unlock(&ota_mutex);

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

/*===========================================================================*/
/* OTA Worker Thread                                                         */
/*===========================================================================*/

/**
 * @brief OTA worker thread entry point
 *
 * Decouples flash operations (erase, write, validate) from transport threads
 * (HTTP server, BLE GATT callbacks, USB CDC handlers).  Transports post
 * commands and firmware bytes via ota_cmd_q / ota_data_pipe and return
 * immediately; only ota_finalize_update() blocks (briefly) waiting for the
 * worker to complete validation.
 *
 * Flow per firmware update:
 *   1. CMD_START  -> do_start_update()  (may block 2-30 s for flash erase)
 *   2. pipe data  -> do_write_chunk()   (fast, microseconds per 4 KB page)
 *   3. CMD_FINALIZE -> drain pipe -> do_finalize_update() -> k_sem_give()
 */
static void ota_worker_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    struct ota_cmd cmd;
    /* Stack buffer for pipe reads -- write_buffer[] lives in BSS, not here */
    uint8_t chunk[512];

    LOG_INF("OTA worker thread started");

    while (1)
    {
        /* ------------------------------------------------------------------ */
        /* Wait for a command from any transport                               */
        /* ------------------------------------------------------------------ */
        k_msgq_get(&ota_cmd_q, &cmd, K_FOREVER);

        if (cmd.type != OTA_CMD_START)
        {
            /* Spurious FINALIZE/ABORT with no active transfer */
            LOG_WRN("OTA worker: CMD %d without active transfer", cmd.type);
            ota_thread_result = OTA_ERROR_NOT_INITIALIZED;
            k_sem_give(&ota_done_sem);
            continue;
        }

        /* ------------------------------------------------------------------ */
        /* Phase 1: Start update -- flash erase (may block 2-30 s)            */
        /* ------------------------------------------------------------------ */
        enum ota_result start_res = do_start_update(cmd.size);
        if (start_res != OTA_OK)
        {
            /* Error state already set by do_start_update via set_error() */
            ota_thread_result = start_res;
            k_sem_give(&ota_done_sem);
            /* Discard any bytes already queued in the pipe */
            while (k_pipe_read(&ota_data_pipe, chunk, sizeof(chunk),
                               K_NO_WAIT) > 0)
            {
            }
            continue;
        }

        /* ------------------------------------------------------------------ */
        /* Phase 2: Receive loop -- write pipe data, watch for FINALIZE/ABORT */
        /* ------------------------------------------------------------------ */
        bool finished = false;
        while (!finished)
        {

            /* Non-blocking check for a control command */
            struct ota_cmd ctrl;
            if (k_msgq_get(&ota_cmd_q, &ctrl, K_NO_WAIT) == 0)
            {
                bool do_abort = (ctrl.type == OTA_CMD_ABORT);

                /* Drain all remaining bytes from pipe before finalizing */
                ssize_t read;
                while ((read = k_pipe_read(&ota_data_pipe, chunk, sizeof(chunk),
                                           K_NO_WAIT)) > 0)
                {
                    if (!do_abort)
                    {
                        enum ota_result wr = do_write_chunk(chunk, (uint16_t)read);
                        if (wr != OTA_OK)
                        {
                            LOG_ERR("OTA worker: drain write failed: %d", wr);
                            do_abort = true;
                        }
                    }
                }

                if (!do_abort)
                {
                    ota_thread_result = do_finalize_update();
                }
                else
                {
                    do_abort_update();
                    ota_thread_result = OTA_OK;
                }
                k_sem_give(&ota_done_sem);
                finished = true;
                continue;
            }

            /* Read next chunk from pipe -- 100 ms timeout to re-check cmd queue */
            ssize_t read = k_pipe_read(&ota_data_pipe, chunk, sizeof(chunk),
                                       K_MSEC(100));
            if (read > 0)
            {
                enum ota_result wr = do_write_chunk(chunk, (uint16_t)read);
                if (wr != OTA_OK)
                {
                    LOG_ERR("OTA worker: write chunk failed: %d", wr);
                    /* Drain pipe so transport's k_pipe_write unblocks */
                    while (k_pipe_read(&ota_data_pipe, chunk, sizeof(chunk),
                                       K_NO_WAIT) > 0)
                    {
                    }
                    ota_thread_result = wr;
                    k_sem_give(&ota_done_sem);
                    finished = true;
                    /* Drain stale commands (e.g. CMD_FINALIZE that will never happen) */
                    struct ota_cmd stale;
                    while (k_msgq_get(&ota_cmd_q, &stale, K_NO_WAIT) == 0)
                    {
                    }
                }
            }
            /* ret == -EAGAIN: pipe empty for 100 ms -- loop back to check cmd queue */
        }
    }
}

/**
 * @brief Transport callback for firmware data
 *
 * Called directly from the transport layer for zero-copy data dispatch.
 * Handles flash writes synchronously with 4KB alignment buffer.
 *
 * @param data Pointer to chunk data
 * @param len Length of data chunk
 * @param info Chunk metadata
 * @return 0 on success, negative errno on failure
 */
static int ota_data_callback(const uint8_t *data, size_t len,
                             const struct transport_chunk_info *info)
{
    if (!info)
    {
        return -EINVAL;
    }

    /* Handle transfer lifecycle events */
    if (info->flags & TRANSPORT_FLAG_CHUNK_START)
    {
        LOG_INF("OTA: Transfer started, size=%u", info->total_size);
        enum ota_result result = ota_start_update(info->total_size);
        return (result == OTA_OK) ? 0 : -EIO;
    }

    if (info->flags & TRANSPORT_FLAG_ABORT)
    {
        LOG_WRN("OTA: Transfer aborted");
        ota_abort_update();
        return 0;
    }

    if (info->flags & TRANSPORT_FLAG_CHUNK_END)
    {
        LOG_INF("OTA: Transfer complete, finalizing...");
        enum ota_result result = ota_finalize_update();
        return (result == OTA_OK) ? 0 : -EIO;
    }

    /* Handle data chunks */
    if (data && len > 0)
    {
        if (len > 65535)
        {
            LOG_ERR("OTA: Chunk too large: %zu", len);
            return -EINVAL;
        }

        enum ota_result result = ota_write_chunk(data, len);
        if (result != OTA_OK)
        {
            LOG_ERR("OTA: Write failed: %s", ota_result_to_string(result));
            return -EIO;
        }
    }

    return 0;
}

/* Public API */
int ota_manager_init(void)
{
    memset(&ota_status, 0, sizeof(ota_status));
    ota_status.state = OTA_STATE_IDLE;
    strcpy(ota_status.status_message, "Initialized");

    /* Initialize transport interface */
    transport_init();

    /* Register as handler for FIRMWARE data type */
    ota_transport_handler_id = transport_register_handler(
        TRANSPORT_DATA_FIRMWARE,
        ota_data_callback,
        NULL,
        0 /* Highest priority */
    );

    if (ota_transport_handler_id < 0)
    {
        LOG_ERR("Failed to register OTA transport handler: %d",
                ota_transport_handler_id);
        /* Continue anyway - direct API still works */
    }

    /* Start the dedicated OTA worker thread */
    ota_worker_tid = k_thread_create(
        &ota_worker_data,
        ota_worker_stack,
        K_THREAD_STACK_SIZEOF(ota_worker_stack),
        ota_worker_fn,
        NULL, NULL, NULL,
        OTA_THREAD_PRIORITY,
        0,
        K_NO_WAIT);
    k_thread_name_set(ota_worker_tid, "ota_worker");

    LOG_INF("OTA Manager ready (worker thread mode)");
    return 0;
}

/**
 * Post CMD_START to the worker thread and return immediately.
 * Sets state to IN_PROGRESS so ota_write_chunk() proceeds before the
 * worker thread transitions state to RECEIVING after flash erase.
 */
enum ota_result ota_start_update(size_t expected_size)
{
    k_mutex_lock(&ota_mutex, K_FOREVER);
    enum ota_state cur = ota_status.state;
    k_mutex_unlock(&ota_mutex);

    if (cur != OTA_STATE_IDLE && cur != OTA_STATE_ERROR &&
        cur != OTA_STATE_COMPLETE)
    {
        return OTA_ERROR_ALREADY_IN_PROGRESS;
    }

    /* Mark as in-progress immediately so ota_write_chunk() accepts data
     * before the worker thread has set state to RECEIVING after erase. */
    k_mutex_lock(&ota_mutex, K_FOREVER);
    ota_status.state = OTA_STATE_IN_PROGRESS;
    k_mutex_unlock(&ota_mutex);

    struct ota_cmd cmd = {.type = OTA_CMD_START, .size = (uint32_t)expected_size};
    int ret = k_msgq_put(&ota_cmd_q, &cmd, K_MSEC(100));
    if (ret != 0)
    {
        k_mutex_lock(&ota_mutex, K_FOREVER);
        ota_status.state = OTA_STATE_IDLE;
        k_mutex_unlock(&ota_mutex);
        return OTA_ERROR_ALREADY_IN_PROGRESS;
    }

    return OTA_OK;
}

/**
 * Push a firmware chunk into the data pipe.
 * Blocks the calling thread (HTTP/BLE/USB) for up to 5 s when the pipe is
 * full, providing back-pressure rather than dropping data.
 * Returns OTA_ERROR_TIMEOUT if back-pressure exceeds 5 s, or the last
 * flash write error if the worker thread reported one.
 */
enum ota_result ota_write_chunk(const uint8_t *data, size_t length)
{
    if (!data || length == 0 || length > 65535)
    {
        return OTA_ERROR_INVALID_PARAM;
    }

    /* Surface any flash error set by the worker thread */
    k_mutex_lock(&ota_mutex, K_FOREVER);
    enum ota_state state = ota_status.state;
    enum ota_result last_err = ota_status.last_error;
    k_mutex_unlock(&ota_mutex);

    if (state == OTA_STATE_ERROR)
    {
        return (last_err != OTA_OK) ? last_err : OTA_ERROR_NOT_INITIALIZED;
    }
    if (state != OTA_STATE_IN_PROGRESS && state != OTA_STATE_RECEIVING)
    {
        return OTA_ERROR_NOT_INITIALIZED;
    }

    /* Push into pipe; blocks until all bytes are queued or timeout */
    ssize_t written = k_pipe_write(&ota_data_pipe, data, length, K_MSEC(5000));
    if (written != (ssize_t)length)
    {
        LOG_ERR("OTA pipe write timeout: written=%zd of %zu", written, length);
        return OTA_ERROR_TIMEOUT;
    }

    return OTA_OK;
}

/**
 * Signal the worker to finalize and wait for the result.
 * The worker drains the data pipe, validates the image, and marks it
 * for next boot before returning the result via ota_thread_result.
 */
enum ota_result ota_finalize_update(void)
{
    /* Drain any stale semaphore count from a previous error path */
    k_sem_take(&ota_done_sem, K_NO_WAIT);

    struct ota_cmd cmd = {.type = OTA_CMD_FINALIZE, .size = 0};
    int ret = k_msgq_put(&ota_cmd_q, &cmd, K_MSEC(100));
    if (ret != 0)
    {
        LOG_ERR("OTA finalize: failed to post command");
        return OTA_ERROR_NOT_INITIALIZED;
    }

    /* Block until worker signals completion */
    k_sem_take(&ota_done_sem, K_FOREVER);
    return ota_thread_result;
}

/**
 * Request an abort.  Fire-and-forget: the worker will drain the pipe
 * and call do_abort_update() asynchronously.
 * Falls back to a direct synchronous abort if the command queue is full.
 */
enum ota_result ota_abort_update(void)
{
    struct ota_cmd cmd = {.type = OTA_CMD_ABORT, .size = 0};
    if (k_msgq_put(&ota_cmd_q, &cmd, K_MSEC(100)) != 0)
    {
        LOG_WRN("OTA abort: cmd queue full, aborting directly");
        return do_abort_update();
    }
    return OTA_OK;
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
    return do_confirm_firmware();
}

enum ota_result ota_request_rollback(void)
{
    /* If the current image has not yet been confirmed (test-mode boot), a
     * cold reset will cause MCUboot to automatically revert to the previous
     * confirmed image.  If the image was already confirmed, rollback is not
     * possible without reinstalling the old firmware via a new OTA cycle.
     * Either way, this call marks the state and lets the caller trigger the
     * reboot via sys_reboot() or the shell. */
    k_mutex_lock(&ota_mutex, K_FOREVER);
    ota_status.state = OTA_STATE_IDLE;
    strncpy(ota_status.status_message, "Rollback requested — reboot to apply",
            sizeof(ota_status.status_message) - 1);
    ota_status.status_message[sizeof(ota_status.status_message) - 1] = '\0';
    k_mutex_unlock(&ota_mutex);

    LOG_INF("OTA rollback requested (reboot to apply)");
    return OTA_OK;
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
        [OTA_STATE_IN_PROGRESS] = "Starting",
        [OTA_STATE_RECEIVING] = "Receiving",
        [OTA_STATE_VALIDATING] = "Validating",
        [OTA_STATE_INSTALLING] = "Installing",
        [OTA_STATE_COMPLETE] = "Complete",
        [OTA_STATE_ERROR] = "Error"};

    return (state < ARRAY_SIZE(strings) && strings[state]) ? strings[state] : "Unknown";
}

void ota_reboot_to_apply_update(uint32_t delay_ms)
{
    LOG_INF("Scheduling reboot in %u ms", delay_ms);
    if (delay_ms > 0)
    {
        k_sleep(K_MSEC(delay_ms));
    }
    sys_reboot(SYS_REBOOT_WARM);
}

/* Enhanced shell commands */
static int cmd_ota_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    const struct ota_progress *p = ota_get_progress();

    shell_print(sh, "\n=== OTA Status ===");
    shell_print(sh, "State: %s", ota_state_to_string(p->state));
    shell_print(sh, "Progress: %d%%", p->percentage);

    if (p->total_size > 0)
    {
        shell_print(sh, "Size: %zu / %zu bytes (%.1f MB)",
                    p->bytes_written, p->total_size,
                    (double)p->total_size / (1024.0 * 1024.0));
    }

    if (p->state == OTA_STATE_ERROR)
    {
        shell_print(sh, "Last Error: %s", ota_result_to_string(p->last_error));
    }

    if (strlen(p->status_message) > 0)
    {
        shell_print(sh, "Status: %s", p->status_message);
    }

    shell_print(sh, "\nNote: LittleFS storage is separate from firmware slots");
    shell_print(sh, "      and NOT erased during OTA updates");

    return 0;
}

static int cmd_ota_confirm(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    enum ota_result result = ota_confirm_firmware();
    shell_print(sh, result == OTA_OK ? "Firmware confirmed" : "Failed: %s",
                ota_result_to_string(result));
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ota_cmds,
                               SHELL_CMD(status, NULL, "OTA status", cmd_ota_status),
                               SHELL_CMD(confirm, NULL, "Confirm firmware", cmd_ota_confirm),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ota, &ota_cmds, "OTA management", NULL);