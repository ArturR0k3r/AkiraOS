/**
* @file settings.h
* @brief Key-value settings storage with NVS backend
*
*
 * @stability stable
 * @since 1.4
 */

#ifndef AKIRA_SETTINGS_H
#define AKIRA_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <settings/system_settings.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_COUNTER_ID 0 // Position in NVS to store count of settings
#define SETTINGS_START_ID 1 // Starting ID for settings entries in NVS

#define MAX_KEYS CONFIG_AKIRA_SETTINGS_MAX_KEYS
#define MAX_KEY_LEN CONFIG_AKIRA_SETTINGS_MAX_KEY_LEN
#define MAX_VALUE_LEN CONFIG_AKIRA_SETTINGS_MAX_VALUE_LEN

#define MINIMUM_ENCRYPTED_LEN 32

/* Helper macro to convert hex string to byte array */
#define HEX_TO_BYTE(h1, h2) \
    ((((h1) >= '0' && (h1) <= '9') ? ((h1) - '0') : \
      ((h1) >= 'a' && (h1) <= 'f') ? ((h1) - 'a' + 10) : \
      ((h1) >= 'A' && (h1) <= 'F') ? ((h1) - 'A' + 10) : 0) << 4 | \
     (((h2) >= '0' && (h2) <= '9') ? ((h2) - '0') : \
      ((h2) >= 'a' && (h2) <= 'f') ? ((h2) - 'a' + 10) : \
      ((h2) >= 'A' && (h2) <= 'F') ? ((h2) - 'A' + 10) : 0))

// Iterator for listing all keys
typedef struct {
    uint16_t index;  
    uint16_t count;
    char* key;   
    char* value; 
} settings_iterator_t;

typedef enum {
    AKIRA_SETTINGS_OP_SET = 0,
    AKIRA_SETTINGS_OP_GET,
    AKIRA_SETTINGS_OP_DELETE,
    AKIRA_SETTINGS_OP_CLEAR
} settings_op_type_t;

typedef struct{
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
    uint8_t encrypted;
} settings_entry_t;


typedef void (*settings_wq_callback_t)(int result, void *user_data);

/**
 * Initialze settings
 * 
 * @return 0 on success, negative on failure
*/
int akira_settings_init(void);

/**
 *  Get value from Key
 * 
 * @param key - Key form which to get value
 * @param value - Output buffer for value
 * @param max_len - Size of output buffer
 * @return 0 on success, negative on error
 */
int akira_settings_get(const char *key, char *value, size_t max_len);


/**
 * Set Key to Value
 * 
 * @param key - Key
 * @param value - Value to store
 * @param is_encrypted - Encryption flag
 * @return 0 on success, negative on error
 */
int akira_settings_set(const char *key, const char *value, uint8_t is_encrypted);

/**
 * Remove the key and the value associated
 *
 * @param key - Key to remove
 * @return 0 on success, negative on error
 */
int akira_settings_delete(const char *key);

/**
 * Monotonic counter incremented on every successful settings mutation, from
 * any writer (shell, settings UI, HTTP API, BLE companion).
 *
 * Intended for hot loops that would otherwise re-read NVS every tick just to
 * notice a rare change: cache the values, remember the generation, and re-read
 * only when it moves.  An NVS read walks flash — real energy, and it contends
 * for the SPI0 bus shared with PSRAM.
 *
 * @return current generation
 */
unsigned int akira_settings_get_generation(void);

/**
 * List all settings
 * 
 * @param iter - Iterator buffer to get Keys and Values
 * @return 0 on success, 1 when done, negative on error
 */
int akira_settings_list(settings_iterator_t *iter);


/**
 * Async set Key to Value
 * 
 * @param key - Key
 * @param value - Value to store
 * @param callback - Callback function when operation is done
 * @param user_data - User data for callback
 * @return 0 on success, negative on error
 */
int akira_settings_set_async(const char *key, const char *value, settings_wq_callback_t callback, void *user_data, uint8_t is_encrypted);

/**
 * Async delete Key
 * 
 * @param key - Key to remove
 * @param callback - Callback function when operation is done
 * @param user_data - User data for callback
 * @return 0 on success, negative on error
 */
int akira_settings_delete_async(const char *key,  settings_wq_callback_t callback, void *user_data);

/**
 * Settings for a WASM app, in a namespace of its own.
 *
 * @p key is stored as "a/<ns>/<key>", where <ns> is bound to the exact app
 * name, so an app can neither read nor change the OS's keys or another app's.
 * Keys may be up to MAX_KEY_LEN - 6 characters, values up to
 * MAX_VALUE_LEN - 1. New keys are limited per app, for all apps together and
 * by MAX_KEYS, so apps can't fill the partition the OS's settings live in.
 *
 * Safe to call from a thread that may be stopped or aborted while it waits:
 * the request owns copies of everything, the wait is bounded by @p timeout
 * and ends early once @p give_up returns true (checked every 50 ms). A request
 * left behind still runs and frees itself, so a SET or DELETE that returned
 * -ETIMEDOUT or -ECANCELED may still take effect.
 *
 * @param op      AKIRA_SETTINGS_OP_GET, _SET or _DELETE
 * @param app     the app's name
 * @param value   value for SET, else NULL
 * @param out     GET: receives the NUL-terminated value (truncated to fit)
 * @return 0; -ENOENT (no such key), -ENAMETOOLONG, -E2BIG (value),
 *         -ENOSPC (key quota), -EBUSY (too many app requests queued),
 *         -ETIMEDOUT / -ECANCELED (gave up waiting), or a storage error.
 */
int akira_settings_app_call(settings_op_type_t op, const char *app,
                            const char *key, const char *value,
                            char *out, size_t out_len, k_timeout_t timeout,
                            bool (*give_up)(void *ctx), void *ctx);

/**
 * Delete every setting app @p app stored through akira_settings_app_call().
 * For the app manager, when an app is uninstalled. Waits like the other
 * synchronous calls.
 *
 * @return 0 (also when the app has none), or a storage error.
 */
int akira_settings_app_purge(const char *app);

#ifdef __cplusplus
}
#endif

#endif //AKIRA_SETTINGS_H