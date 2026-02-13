/**
* @file settings.h
* @brief Key-value settings storage with NVS backend
*
*
*/

#ifndef AKIRA_SETTINGS_H
#define AKIRA_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

// Special keys for wifi
#define AKIRA_SETTINGS_WIFI_SSID_KEY "wifi/ssid"
#define AKIRA_SETTINGS_WIFI_PSK_KEY "wifi/psk"


#define SETTINGS_COUNTER_ID 0 // Position in NVS to store count of settings
#define SETTINGS_START_ID 1 // Starting ID for settings entries in NVS

#define MAX_KEYS CONFIG_AKIRA_SETTINGS_MAX_KEYS
#define MAX_KEY_LEN CONFIG_AKIRA_SETTINGS_MAX_KEY_LEN
#define MAX_VALUE_LEN CONFIG_AKIRA_SETTINGS_MAX_VALUE_LEN

#define MAX_NAMESPACE_LEN 48
#define MAX_FILEPATH_LEN 96

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

typedef enum{
    AKIRA_SETTINGS_STORAGE_FLASH = 0,
    AKIRA_SETTINGS_STORAGE_SD,
    AKIRA_SETTINGS_STORAGE_AUTO
} settings_storage_type_t;

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

#ifdef __cplusplus
}
#endif

#endif //AKIRA_SETTINGS_H