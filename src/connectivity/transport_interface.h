/**
 * @file transport_interface.h
 * @brief Callback-based Transport Layer Interface
 *
 * Provides a unified, zero-copy data dispatch mechanism for different
 * data types (WASM apps, firmware, files, config). Uses O(1) lookup
 * with a thread-safe registry supporting up to 8 handlers.
 */

#ifndef TRANSPORT_INTERFACE_H
#define TRANSPORT_INTERFACE_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Data types supported by the transport layer
 */
enum transport_data_type {
    TRANSPORT_DATA_WASM_APP = 0,    /**< WebAssembly application binary */
    TRANSPORT_DATA_FIRMWARE = 1,    /**< Firmware update image */
    TRANSPORT_DATA_FILE = 2,        /**< Generic file transfer */
    TRANSPORT_DATA_CONFIG = 3,      /**< Configuration data */
    TRANSPORT_DATA_TYPE_COUNT       /**< Number of data types (for array sizing) */
};

/**
 * @brief Transport operation flags
 */
enum transport_flags {
    TRANSPORT_FLAG_NONE = 0,
    TRANSPORT_FLAG_CHUNK_START = (1 << 0),  /**< First chunk of transfer */
    TRANSPORT_FLAG_CHUNK_END = (1 << 1),    /**< Final chunk of transfer */
    TRANSPORT_FLAG_ABORT = (1 << 2),        /**< Transfer aborted */
};

/**
 * @brief Transport chunk metadata
 *
 * Provides context about the incoming data chunk for handlers.
 */
struct transport_chunk_info {
    enum transport_data_type type;      /**< Type of data being transferred */
    uint32_t total_size;                /**< Total expected size (0 if unknown) */
    uint32_t offset;                    /**< Current offset in the transfer */
    uint32_t flags;                     /**< Transport flags */
    const char *name;                   /**< Optional name/identifier */
    void *user_data;                    /**< Handler-specific context */
};

/**
 * @brief Transport data callback type
 *
 * Called when data is available for processing. Handlers should process
 * data synchronously for zero-copy semantics. Data pointer is only valid
 * during the callback invocation.
 *
 * @param data Pointer to chunk data (read-only, do not cache)
 * @param len Length of data chunk in bytes
 * @param info Chunk metadata and context
 * @return 0 on success, negative errno on failure
 *         -EAGAIN: temporary failure, may retry
 *         -ENOMEM: out of memory
 *         -ENOSPC: insufficient storage space
 *         -EINVAL: invalid data format
 */
typedef int (*transport_data_cb_t)(const uint8_t *data, size_t len,
                                   const struct transport_chunk_info *info);

/**
 * @brief Handler registration entry
 */
struct transport_handler {
    transport_data_cb_t callback;       /**< Data callback function */
    void *user_data;                    /**< User context passed to callback */
    uint8_t priority;                   /**< Handler priority (0 = highest) */
    bool active;                        /**< Handler is currently in use */
};

/**
 * @brief Transport statistics
 */
struct transport_stats {
    uint32_t total_bytes;               /**< Total bytes dispatched */
    uint32_t total_chunks;              /**< Total chunks processed */
    uint32_t errors;                    /**< Total error count */
    uint32_t dispatch_latency_us;       /**< Last dispatch latency in microseconds */
};

/**
 * @brief Initialize the transport interface
 *
 * Must be called before any other transport functions.
 *
 * @return 0 on success, negative errno on failure
 */
int transport_init(void);

/**
 * @brief Register a handler for a specific data type
 *
 * Multiple handlers can be registered per type. Each type supports
 * up to 2 handlers (8 total across all types) with O(1) lookup.
 *
 * @param type Data type to register for
 * @param callback Function to call when data arrives
 * @param user_data Context passed to callback
 * @param priority Handler priority (0 = highest, called first)
 * @return Handler ID (>= 0) on success, negative errno on failure
 *         -EINVAL: invalid parameters
 *         -ENOSPC: registry full
 *         -EALREADY: callback already registered for this type
 */
int transport_register_handler(enum transport_data_type type,
                               transport_data_cb_t callback,
                               void *user_data,
                               uint8_t priority);

/**
 * @brief Unregister a previously registered handler
 *
 * @param handler_id Handler ID returned by transport_register_handler
 * @return 0 on success, negative errno on failure
 */
int transport_unregister_handler(int handler_id);

/**
 * @brief Notify all registered handlers of incoming data
 *
 * Dispatches data to all handlers registered for the specified type,
 * in priority order. Uses zero-copy semantics - data pointer is only
 * valid during callback execution.
 *
 * @param type Data type being notified
 * @param data Pointer to data chunk
 * @param len Length of data chunk
 * @param info Additional chunk metadata
 * @return 0 on success, negative errno on first handler failure
 */
int transport_notify(enum transport_data_type type,
                     const uint8_t *data,
                     size_t len,
                     const struct transport_chunk_info *info);

/**
 * @brief Signal start of a new transfer
 *
 * Notifies handlers that a new transfer is beginning.
 *
 * @param type Data type being transferred
 * @param total_size Expected total size (0 if unknown)
 * @param name Optional name/identifier for the transfer
 * @return 0 on success, negative errno on failure
 */
int transport_begin(enum transport_data_type type,
                    uint32_t total_size,
                    const char *name);

/**
 * @brief Signal end of a transfer
 *
 * Notifies handlers that the transfer is complete.
 *
 * @param type Data type that finished
 * @param success True if transfer completed successfully
 * @return 0 on success, negative errno on failure
 */
int transport_end(enum transport_data_type type, bool success);

/**
 * @brief Abort an ongoing transfer
 *
 * Notifies handlers to abort and clean up.
 *
 * @param type Data type to abort
 * @return 0 on success, negative errno on failure
 */
int transport_abort(enum transport_data_type type);

/**
 * @brief Check if a transfer is in progress
 *
 * @param type Data type to check
 * @return true if transfer is active
 */
bool transport_is_active(enum transport_data_type type);

/**
 * @brief Get transport statistics
 *
 * @param type Data type to get stats for, or -1 for aggregate
 * @param stats Output buffer for statistics
 * @return 0 on success, negative errno on failure
 */
int transport_get_stats(int type, struct transport_stats *stats);

/**
 * @brief Convert data type to string
 *
 * @param type Data type value
 * @return Human-readable string
 */
const char *transport_type_to_string(enum transport_data_type type);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_INTERFACE_H */
