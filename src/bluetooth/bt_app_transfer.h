/**
 * @file bt_app_transfer.h
 * @brief BLE WASM App Transfer Service
 *
 * GATT service for receiving WASM applications over BLE.
 * Supports chunked transfer with CRC32 validation.
 *
 * Service UUID: 0xAK01 (custom)
 * Characteristics:
 *   - RX_DATA:   Write without response - receives app chunks
 *   - TX_STATUS: Notify - transfer status updates
 *   - CONTROL:   Write - transfer control (start/abort/commit)
 */

#ifndef BT_APP_TRANSFER_H
#define BT_APP_TRANSFER_H

#include <zephyr/types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Transfer state machine states
 */
typedef enum {
    BT_APP_XFER_IDLE,       /**< Ready to receive new transfer */
    BT_APP_XFER_RECEIVING,  /**< Currently receiving chunks */
    BT_APP_XFER_VALIDATING, /**< Validating received data */
    BT_APP_XFER_INSTALLING, /**< Installing to app manager */
    BT_APP_XFER_COMPLETE,   /**< Transfer complete, success */
    BT_APP_XFER_ERROR       /**< Transfer failed */
} bt_app_xfer_state_t;

/**
 * @brief Control commands from BLE client
 */
typedef enum {
    BT_APP_CMD_START  = 0x01,  /**< Start new transfer */
    BT_APP_CMD_ABORT  = 0x02,  /**< Abort current transfer */
    BT_APP_CMD_COMMIT = 0x03,  /**< Finalize and install */
    BT_APP_CMD_STATUS = 0x04   /**< Request status update */
} bt_app_cmd_t;

/**
 * @brief Status codes sent to client
 */
typedef enum {
    BT_APP_STATUS_OK           = 0x00,
    BT_APP_STATUS_ERROR        = 0x01,
    BT_APP_STATUS_BUSY         = 0x02,
    BT_APP_STATUS_CRC_FAIL     = 0x03,
    BT_APP_STATUS_SIZE_ERROR   = 0x04,
    BT_APP_STATUS_INSTALL_FAIL = 0x05,
    BT_APP_STATUS_NO_SPACE     = 0x06
} bt_app_status_t;

/**
 * @brief Transfer header (sent with START command)
 *
 * Client sends: [CMD_START][app_name (32)][total_size (4)][crc32 (4)]
 */
struct bt_app_xfer_header {
    char name[32];          /**< App name (null terminated) */
    uint32_t total_size;    /**< Total WASM file size */
    uint32_t expected_crc;  /**< Expected CRC32 of complete file */
};

/**
 * @brief Transfer progress info
 */
struct bt_app_xfer_progress {
    bt_app_xfer_state_t state;
    char app_name[32];
    uint32_t total_size;
    uint32_t received_bytes;
    uint8_t percent_complete;
};

/**
 * @brief Transfer completion callback
 *
 * @param success True if transfer and install succeeded
 * @param app_name Name of the app
 * @param error Error code if failed
 */
typedef void (*bt_app_xfer_complete_cb_t)(bool success, const char *app_name, int error);

/**
 * @brief Initialize BLE App Transfer service
 *
 * Registers GATT service and prepares for transfers.
 *
 * @return 0 on success, negative error code on failure
 */
int bt_app_transfer_init(void);

/**
 * @brief Get current transfer state
 *
 * @return Current state of the transfer state machine
 */
bt_app_xfer_state_t bt_app_transfer_get_state(void);

/**
 * @brief Get transfer progress
 *
 * @param progress Output progress structure
 * @return 0 on success, -EINVAL if progress is NULL
 */
int bt_app_transfer_get_progress(struct bt_app_xfer_progress *progress);

/**
 * @brief Abort any ongoing transfer
 *
 * Can be called to cancel a transfer in progress.
 */
void bt_app_transfer_abort(void);

/**
 * @brief Check if service is ready for transfer
 *
 * @return true if idle and ready, false if busy
 */
bool bt_app_transfer_is_ready(void);

/**
 * @brief Register completion callback
 *
 * @param callback Function to call when transfer completes
 */
void bt_app_transfer_set_callback(bt_app_xfer_complete_cb_t callback);

#ifdef __cplusplus
}
#endif

#endif /* BT_APP_TRANSFER_H */
