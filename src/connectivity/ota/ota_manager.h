/**
 * @file ota_manager.h
 * @brief OTA (Over-The-Air) Update Manager for ESP32 Device
 *
 * Manages firmware updates, validation, and rollback functionality
 * with MCUboot integration and web-based upload support.
 */

#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

/* OTA Configuration */
#define OTA_THREAD_STACK_SIZE 4096
#define OTA_THREAD_PRIORITY 6
#define OTA_MAX_CHUNK_SIZE 4096
#define OTA_PROGRESS_REPORT_SIZE (64 * 1024) // Report every 64KB

/* OTA States */
enum ota_state
{
    OTA_STATE_IDLE = 0,
    OTA_STATE_IN_PROGRESS,
    OTA_STATE_RECEIVING,
    OTA_STATE_VALIDATING,
    OTA_STATE_INSTALLING,
    OTA_STATE_COMPLETE,
    OTA_STATE_ERROR
};

/* OTA Result codes */
enum ota_result
{
    OTA_OK = 0,
    OTA_ERROR_INVALID_PARAM = -1,
    OTA_ERROR_NOT_INITIALIZED = -2,
    OTA_ERROR_ALREADY_IN_PROGRESS = -3,
    OTA_ERROR_FLASH_OPEN_FAILED = -4,
    OTA_ERROR_FLASH_ERASE_FAILED = -5,
    OTA_ERROR_FLASH_WRITE_FAILED = -6,
    OTA_ERROR_INVALID_IMAGE = -7,
    OTA_ERROR_SIGNATURE_VERIFICATION = -8,
    OTA_ERROR_INSUFFICIENT_SPACE = -9,
    OTA_ERROR_TIMEOUT = -10,
    OTA_ERROR_BOOT_REQUEST_FAILED = -11
};

/* OTA Transport Interface */
typedef struct ota_transport
{
    const char *name;
    int (*start)(void *user_data);
    int (*stop)(void *user_data);
    int (*send_chunk)(const uint8_t *data, size_t len, void *user_data);
    int (*report_progress)(uint8_t percent, void *user_data);
    void *user_data;
} ota_transport_t;

/* Register/unregister transport */
int ota_manager_register_transport(const ota_transport_t *transport);
int ota_manager_unregister_transport(const char *name);

/* OTA Progress information */
struct ota_progress
{
    enum ota_state state;
    size_t total_size;
    size_t bytes_written;
    uint8_t percentage;
    enum ota_result last_error;
    char status_message[128];
};

/* OTA Image information */
struct ota_image_info
{
    uint32_t magic;
    uint32_t load_addr;
    uint16_t hdr_size;
    uint16_t protect_tlv_size;
    uint32_t img_size;
    uint32_t flags;
    struct
    {
        uint8_t major;
        uint8_t minor;
        uint16_t revision;
        uint32_t build_num;
    } version;
    uint32_t reserved;
};

/**
 * @brief OTA progress callback function type
 *
 * Called when OTA progress changes or errors occur
 *
 * @param progress Current progress information
 * @param user_data User-provided callback data
 */
typedef void (*ota_progress_cb_t)(const struct ota_progress *progress, void *user_data);

/**
 * @brief Initialize the OTA manager
 *
 * Starts the OTA thread and initializes flash partitions
 *
 * @return 0 on success, negative error code on failure
 */
int ota_manager_init(void);

/**
 * @brief Start OTA update process
 *
 * Prepares the secondary slot for receiving new firmware
 *
 * @param expected_size Expected size of firmware image (0 = unknown)
 * @return ota_result code
 */
enum ota_result ota_start_update(size_t expected_size);

/**
 * @brief Write firmware data chunk
 *
 * @param data Firmware data chunk
 * @param length Length of data chunk
 * @return ota_result code
 */
enum ota_result ota_write_chunk(const uint8_t *data, size_t length);

/**
 * @brief Finalize OTA update
 *
 * Validates the firmware image and marks it for next boot
 *
 * @return ota_result code
 */
enum ota_result ota_finalize_update(void);

/**
 * @brief Abort ongoing OTA update
 *
 * @return ota_result code
 */
enum ota_result ota_abort_update(void);

/**
 * @brief Get current OTA progress
 *
 * @return Pointer to current progress information
 */
const struct ota_progress *ota_get_progress(void);

/**
 * @brief Confirm current firmware as permanent
 *
 * Prevents automatic rollback on next boot
 *
 * @return ota_result code
 */
enum ota_result ota_confirm_firmware(void);

/**
 * @brief Request rollback to previous firmware
 *
 * Schedules rollback on next reboot
 *
 * @return ota_result code
 */
enum ota_result ota_request_rollback(void);

/**
 * @brief Get image information from flash slot
 *
 * @param slot 0 = primary, 1 = secondary
 * @param info Output buffer for image information
 * @return 0 on success, negative error code on failure
 */
int ota_get_image_info(int slot, struct ota_image_info *info);

/**
 * @brief Register progress callback
 *
 * @param callback Callback function
 * @param user_data User data to pass to callback
 * @return ota_result code
 */
enum ota_result ota_register_progress_callback(ota_progress_cb_t callback, void *user_data);

/**
 * @brief Check if OTA update is in progress
 *
 * @return true if update is active
 */
bool ota_is_update_in_progress(void);

/**
 * @brief Get flash slot sizes
 *
 * @param primary_size Output for primary slot size
 * @param secondary_size Output for secondary slot size
 * @return 0 on success, negative error code on failure
 */
int ota_get_slot_sizes(size_t *primary_size, size_t *secondary_size);

/**
 * @brief Format OTA result code as string
 *
 * @param result OTA result code
 * @return Human-readable error string
 */
const char *ota_result_to_string(enum ota_result result);

/**
 * @brief Format OTA state as string
 *
 * @param state OTA state
 * @return Human-readable state string
 */
const char *ota_state_to_string(enum ota_state state);

/**
 * @brief Reboot system to apply firmware update
 *
 * Should be called after successful ota_finalize_update()
 *
 * @param delay_ms Delay before reboot in milliseconds
 */
void ota_reboot_to_apply_update(uint32_t delay_ms);

#endif /* OTA_MANAGER_H */