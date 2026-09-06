#include "settings.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/base64.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef CONFIG_AKIRA_SETTINGS_ENCRYPTION
#include <mbedtls/gcm.h>
#include <mbedtls/platform.h>
#include <zephyr/random/random.h>
#endif
#include "lib/mem_helper.h"

LOG_MODULE_REGISTER(akira_settings, CONFIG_LOG_DEFAULT_LEVEL);

#ifdef CONFIG_AKIRA_SETTINGS_ENCRYPTION

#define ENCRYPTION_MAGIC 0xAE01
#define MAGIC_SIZE 2
#define IV_SIZE 12
#define TAG_SIZE 16

static struct
{
    mbedtls_gcm_context gcm;
    bool initialized;
} crypto_ctx = {
    .initialized = false};

static uint8_t ENCRYPTION_KEY[32] __attribute__((section(".ext_ram.bss")));

static int parse_hex_key(void)
{
    const char *hex = CONFIG_AKIRA_SETTINGS_ENCRYPTION_KEY_HEX;

    if (strlen(hex) != 64)
    {
        return -EINVAL;
    }

    for (int i = 0; i < 32; i++)
    {
        ENCRYPTION_KEY[i] = HEX_TO_BYTE(hex[i * 2], hex[i * 2 + 1]);
    }

    return 0;
}

/**
 * @brief Weak hook: provide the 32-byte AES-256 NVS encryption key.
 *
 * The default implementation parses CONFIG_AKIRA_SETTINGS_ENCRYPTION_KEY_HEX.
 * AkiraPlatform overrides this with a strong implementation that reads the key
 * from a hardware secure element (ATECC608B, SE050) or ESP32 eFuse, ensuring
 * key material never appears as a compile-time constant in firmware.
 *
 * @param key_out  Output buffer — must be exactly 32 bytes.
 * @return 0 on success, negative errno on failure.
 */
__weak int akira_settings_get_encryption_key(uint8_t *key_out)
{
    int ret = parse_hex_key();
    if (ret != 0)
    {
        return ret;
    }
    memcpy(key_out, ENCRYPTION_KEY, 32);
    return 0;
}

static int crypto_init(void)
{
    if (crypto_ctx.initialized)
    {
        return 0;
    }

    int ret = akira_settings_get_encryption_key(ENCRYPTION_KEY);
    if (ret != 0)
    {
        LOG_ERR("Failed to obtain encryption key");
        return ret;
    }

    mbedtls_gcm_init(&crypto_ctx.gcm);

    ret = mbedtls_gcm_setkey(&crypto_ctx.gcm,
                             MBEDTLS_CIPHER_ID_AES,
                             ENCRYPTION_KEY,
                             256);

    if (ret != 0)
    {
        LOG_ERR("Failed to set encryption key: %d", ret);
        mbedtls_gcm_free(&crypto_ctx.gcm);
        return -EINVAL;
    }

    crypto_ctx.initialized = true;
    LOG_INF("AES-256-GCM encryption initialized (HW accelerated)");

    return 0;
}

static int crypto_encrypt(const char *plaintext, uint8_t *output, size_t max_len)
{
    if (!crypto_ctx.initialized)
    {
        LOG_ERR("Crypto not initialized");
        return -EINVAL;
    }

    if (!plaintext || !output)
    {
        return -EINVAL;
    }

    size_t plain_len = strlen(plaintext);

    size_t required_len = MAGIC_SIZE + IV_SIZE + plain_len + TAG_SIZE;
    if (required_len > max_len)
    {
        LOG_ERR("Buffer too small: need %zu, have %zu", required_len, max_len);
        return -E2BIG;
    }

    output[0] = (ENCRYPTION_MAGIC >> 8) & 0xFF;
    output[1] = ENCRYPTION_MAGIC & 0xFF;

    uint8_t iv[IV_SIZE];
    sys_rand_get(iv, IV_SIZE);
    memcpy(output + MAGIC_SIZE, iv, IV_SIZE);

    uint8_t tag[TAG_SIZE];

    int ret = mbedtls_gcm_crypt_and_tag(
        &crypto_ctx.gcm,
        MBEDTLS_GCM_ENCRYPT,
        plain_len,
        iv, IV_SIZE,
        NULL, 0,
        (const unsigned char *)plaintext,
        output + MAGIC_SIZE + IV_SIZE,
        TAG_SIZE,
        tag);

    if (ret != 0)
    {
        LOG_ERR("Encryption failed: %d", ret);
        return -EIO;
    }

    memcpy(output + MAGIC_SIZE + IV_SIZE + plain_len, tag, TAG_SIZE);

    return required_len;
}

static int crypto_decrypt(const uint8_t *input, size_t input_len,
                          char *output, size_t max_len)
{

    if (input_len < MAGIC_SIZE + IV_SIZE + TAG_SIZE)
    {
        LOG_ERR("Encrypted data too short: %zu", input_len);
        return -EINVAL;
    }

    const uint8_t *iv = input + MAGIC_SIZE;
    const uint8_t *encrypted_data = input + MAGIC_SIZE + IV_SIZE;
    size_t data_len = input_len - MAGIC_SIZE - IV_SIZE - TAG_SIZE;
    const uint8_t *tag = input + MAGIC_SIZE + IV_SIZE + data_len;

    if (data_len >= max_len)
    {
        LOG_ERR("Output buffer too small: need %zu, have %zu", data_len + 1, max_len);
        return -E2BIG;
    }

    int ret = mbedtls_gcm_auth_decrypt(
        &crypto_ctx.gcm,
        data_len,
        iv, IV_SIZE,
        NULL, 0,
        tag, TAG_SIZE,
        encrypted_data,
        (unsigned char *)output);

    if (ret != 0)
    {
        if (ret == MBEDTLS_ERR_GCM_AUTH_FAILED)
        {
            LOG_ERR("Authentication failed - data tampered!");
        }
        else
        {
            LOG_ERR("Decryption failed: %d", ret);
        }
        return -EIO;
    }

    output[data_len] = '\0';

    return 0;
}

#else

static int crypto_init(void)
{
    LOG_INF("Encryption disabled (plaintext mode)");
    return 0;
}

static int crypto_encrypt(const char *plaintext, uint8_t *output, size_t max_len)
{
    LOG_INF("Encryption disabled (plaintext mode)");
    return -ENOTSUP;
}

static int crypto_decrypt(const uint8_t *input, size_t input_len, char *output, size_t max_len)
{
    LOG_INF("Encryption disabled (plaintext mode)");
    return -ENOTSUP;
}

#endif

static K_MUTEX_DEFINE(akira_settings_mutex);

/* Given once akira_settings_init() (APPLICATION level) finishes. The shell's
 * own POST_KERNEL init starts its thread earlier and, under CONFIG_SMP, that
 * thread can run on the other core in parallel with the rest of boot -- so a
 * command typed the instant the prompt appears can otherwise race storage
 * init. submit_settings_work() waits on this instead of failing outright. */
static K_SEM_DEFINE(storage_ready_sem, 0, 1);
#define SETTINGS_INIT_WAIT_MS 3000

/* Bumped by the settings workqueue on every successful mutation (set, delete,
 * clear), from any caller — shell, settings UI, HTTP, BLE companion.  Lets hot
 * loops cache values they would otherwise re-read from NVS on every tick; an
 * NVS read walks flash, which costs both time and energy and contends for the
 * SPI0 bus that PSRAM shares.  See akira_settings_get_generation(). */
atomic_t akira_settings_generation = ATOMIC_INIT(0);

K_THREAD_STACK_DEFINE(work_stack, 2048);

struct akira_setting_work
{
    struct k_work work;
    settings_op_type_t type;
    char *key;
    char *value;
    uint16_t max_len;
    uint8_t encrypted;

    settings_wq_callback_t callback;
    void *user_data;
    struct k_sem *completion_sem;
    int *result_ptr;
};

static struct
{
    struct nvs_fs nvs;
    bool initialized;

    struct k_work_q work_queue;
} storage AKIRA_BULK_BSS = {
    .initialized = false};

/* Compact NVS entries: reads all valid entries, rewrites them sequentially
 * from SETTINGS_START_ID, and updates the counter.  Called at init when
 * holes are detected (e.g. after OTA that wiped the old NVS partition address
 * but left a stale counter value). */
static int compact_entries(uint16_t counter)
{
    /* Heap-allocate to avoid blowing the stack (MAX_KEYS * ~97 bytes). */
    settings_entry_t *buf = k_malloc(sizeof(settings_entry_t) * counter);
    if (!buf)
    {
        LOG_ERR("compact_entries: out of memory");
        return -ENOMEM;
    }

    /* Hold the settings mutex for the entire multi-step read/rewrite sequence
     * so that a concurrent akira_settings_set() cannot interleave and corrupt
     * the NVS partition. */
    k_mutex_lock(&akira_settings_mutex, K_FOREVER);

    uint16_t valid = 0;
    for (uint16_t i = 0; i < counter; i++)
    {
        int r = nvs_read(&storage.nvs, SETTINGS_START_ID + i, &buf[valid], sizeof(settings_entry_t));
        if (r >= 0)
        {
            valid++;
        }
    }

    /* Rewrite valid entries packed from slot 0 */
    for (uint16_t i = 0; i < valid; i++)
    {
        nvs_write(&storage.nvs, SETTINGS_START_ID + i, &buf[i], sizeof(settings_entry_t));
    }
    k_free(buf);

    /* Delete any leftover slots above the new count */
    for (uint16_t i = valid; i < counter; i++)
    {
        nvs_delete(&storage.nvs, SETTINGS_START_ID + i);
    }

    /* Persist updated counter */
    nvs_write(&storage.nvs, SETTINGS_COUNTER_ID, &valid, sizeof(valid));

    k_mutex_unlock(&akira_settings_mutex);

    LOG_INF("NVS compact: %u entries -> %u valid entries", counter, valid);
    return 0;
}

static int settings_get_id(const char *key)
{
    int ret;
    uint16_t counter;
    ret = nvs_read(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter));
    if (ret < 0)
    {
        LOG_WRN("Failed to read SETTINGS_COUNTER_ID (%d)", ret);
        return ret;
    }
    settings_entry_t entry;
    for (uint16_t i = 0; i < counter; i++)
    {
        ret = nvs_read(&storage.nvs, SETTINGS_START_ID + i, &entry, sizeof(entry));
        if (ret < 0)
        {
            if (ret == -ENOENT)
            {
                /* Hole: this slot was deleted or never written — skip it */
                continue;
            }
            LOG_WRN("Failed to read entry at index: %d (%d)", i, ret);
            return ret;
        }
        if (strcmp(entry.key, key) == 0)
        {
            return SETTINGS_START_ID + i;
        }
    }
    return -1;
}

/*===========================================================================*/
/*            Internal Functions                                             */
/*===========================================================================*/

static int init_flash(void)
{
    if (storage.initialized)
    {
        return 0;
    }
    const struct device *flash_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));

    if (!device_is_ready(flash_dev))
    {
        LOG_ERR("Flash device not ready");
        return -ENODEV;
    }

    storage.nvs.flash_device = flash_dev;
    storage.nvs.offset = FIXED_PARTITION_OFFSET(akira_settings_nvs_partition);
    storage.nvs.sector_size = 4096;
    storage.nvs.sector_count = 8;

    int ret = nvs_mount(&storage.nvs);
    if (ret)
    {
        LOG_ERR("NVS mount failed: %d", ret);
        return ret;
    }

    uint16_t counter;
    ret = nvs_read(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter));
    if (ret < 0)
    {
        LOG_INF("Failed to read SETTINGS_COUNTER_ID trying to initialize it(%d)", ret);
        counter = 0;
        ret = nvs_write(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter));
        if (ret < 0)
        {
            LOG_ERR("Failed to initialize SETTINGS_COUNTER_ID (%d)", ret);
            return ret;
        }
        LOG_INF("Initialized SETTINGS_COUNTER_ID to 0");
    }

    LOG_INF("Settings entries in flash: %d", counter);

    /* Heal: if the counter says there are entries but NVS has holes (e.g. after
     * OTA that overwrote the old NVS partition address, leaving a stale counter
     * while the actual entry slots are gone), compact now so every subsequent
     * settings_get_id scan starts from a clean, contiguous list. */
    if (counter > 0)
    {
        bool has_holes = false;
        settings_entry_t probe;
        for (uint16_t i = 0; i < counter; i++)
        {
            if (nvs_read(&storage.nvs, SETTINGS_START_ID + i, &probe, sizeof(probe)) < 0)
            {
                has_holes = true;
                break;
            }
        }
        if (has_holes)
        {
            LOG_WRN("NVS holes detected (stale counter?), compacting entries...");
            compact_entries(counter);
        }
    }

    LOG_INF("Flash type initialized (NVS mounted)");

    return 0;
}

static int settings_set(const char *key, const char *value, uint8_t is_encrypted)
{
    int ret = -1;
    char *b64_value = NULL;

    if (is_encrypted)
    {
#ifndef CONFIG_AKIRA_SETTINGS_ENCRYPTION
        LOG_ERR("Encryption not enabled in build!");
        return -ENOTSUP;
#else
        size_t encrypted_buf_size = MAGIC_SIZE + IV_SIZE + strlen(value) + TAG_SIZE;
        uint8_t *encrypted_buf = k_malloc(encrypted_buf_size);
        if (!encrypted_buf)
        {
            LOG_ERR("Failed to allocate encryption buffer");
            return -ENOMEM;
        }

        int encrypted_len = crypto_encrypt(value, encrypted_buf, encrypted_buf_size);
        if (encrypted_len < 0)
        {
            LOG_ERR("Encryption failed: %d", encrypted_len);
            k_free(encrypted_buf);
            return encrypted_len;
        }

        size_t b64_len = 4 * ((encrypted_len + 2) / 3) + 1;
        b64_value = k_malloc(b64_len);
        if (!b64_value)
        {
            k_free(encrypted_buf);
            return -ENOMEM;
        }

        size_t written;
        int ret = base64_encode(b64_value, b64_len, &written, encrypted_buf, encrypted_len);
        k_free(encrypted_buf);

        if (ret != 0)
        {
            k_free(b64_value);
            return ret;
        }

        if (strlen(b64_value) >= MAX_VALUE_LEN)
        {
            LOG_ERR("Encrypted value too long: %zu >= %d", strlen(b64_value), MAX_VALUE_LEN);
            k_free(b64_value);
            return -E2BIG;
        }

#endif /* CONFIG_AKIRA_SETTINGS_ENCRYPTION */
    }

    settings_entry_t entry;
    strncpy(entry.key, key, sizeof(entry.key) - 1);
    entry.key[sizeof(entry.key) - 1] = '\0';
    // Use b64_value if it exits, that means that its encrypted
    strncpy(entry.value, b64_value ? b64_value : value, sizeof(entry.value) - 1);
    entry.value[sizeof(entry.value) - 1] = '\0';
    entry.encrypted = is_encrypted ? 1 : 0;

    uint16_t counter;
    ret = nvs_read(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter));
    if (ret < 0)
    {
        LOG_WRN("Failed to read SETTINGS_COUNTER_ID (%d)", ret);
        if (b64_value)
            k_free(b64_value);
        return ret;
    }

    int entry_id = settings_get_id(key);
    if (entry_id < 0)
    { // Not found, need to add it
        entry_id = SETTINGS_START_ID + counter;
        ret = nvs_write(&storage.nvs, entry_id, &entry, sizeof(entry));
        if (ret < 0)
        {
            LOG_WRN("Failed to add %s - %s at index %d", key, value, entry_id);
            if (b64_value)
                k_free(b64_value);
            return ret;
        }
        counter++;
        ret = nvs_write(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter));
        if (ret < 0)
        {
            LOG_WRN("Failed to increment counter %d -> %d", (counter - 1), counter);
            return ret;
        }
    }
    else
    { // Overwrite the current value for key
        ret = nvs_write(&storage.nvs, entry_id, &entry, sizeof(entry));
        if (ret < 0)
        {
            LOG_WRN("Failed to change value of %s to %s at index %d", key, value, entry_id);
            if (b64_value)
                k_free(b64_value);
            return ret;
        }
    }
    if (b64_value)
        k_free(b64_value);
    return 0;
}

static int settings_get(const char *key, char *value, size_t max_len)
{
    int ret = -1;

    int entry_id = settings_get_id(key);
    if (entry_id < 0)
    {
        LOG_DBG("Couldn't find key: %s", key);
        return entry_id;
    }

    settings_entry_t entry;
    ret = nvs_read(&storage.nvs, entry_id, &entry, sizeof(entry));
    if (ret < 0)
    {
        LOG_WRN("Failed to read key %s at index %d", key, entry_id);
        return ret;
    }

    if (entry.encrypted)
    {
#ifdef CONFIG_AKIRA_SETTINGS_ENCRYPTION
        uint8_t decoded[MAX_VALUE_LEN];
        size_t decoded_len;
        ret = base64_decode(decoded, sizeof(decoded), &decoded_len,
                            entry.value, strlen(entry.value));

        if (ret != 0)
        {
            LOG_ERR("Base64 decode failed: %d", ret);
            return ret;
        }

        return crypto_decrypt(decoded, decoded_len, value, max_len);
#else
        LOG_ERR("Entry is encrypted but encryption not enabled");
        return -ENOTSUP;
#endif
    }
    else
    {
        strncpy(value, entry.value, max_len - 1);
        value[max_len - 1] = '\0';
    }

    return 0;
}

static int settings_delete(const char *key)
{
    int ret = -1;
    int entry_id = settings_get_id(key);
    if (entry_id < 0)
    {
        LOG_DBG("Couldn't find key: %s", key);
        return entry_id;
    }

    uint16_t counter;
    ret = nvs_read(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter));
    if (ret < 0)
    {
        LOG_INF("Failed to read SETTINGS_COUNTER_ID (%d)", ret);
        return ret;
    }

    uint16_t last_index = counter - 1;
    int last_entry_id = SETTINGS_START_ID + last_index;

    if (entry_id != last_entry_id)
    { // Write the last key to the value we want to remove. This is done to avoid shifting all of the settings.
        settings_entry_t last_entry;
        ret = nvs_read(&storage.nvs, last_entry_id, &last_entry, sizeof(last_entry));
        if (ret < 0)
        {
            LOG_WRN("Failed to read last entry at index: %d (%d)", last_entry_id, ret);
            return ret;
        }

        ret = nvs_write(&storage.nvs, entry_id, &last_entry, sizeof(last_entry));
        if (ret < 0)
        {
            LOG_WRN("Failed to move last entry to index %d (%d)", entry_id, ret);
            return ret;
        }

        ret = nvs_delete(&storage.nvs, last_entry_id);
        if (ret < 0)
        {
            LOG_WRN("Failed to delete last entry at index %d (%d)", last_entry_id, ret);
            return ret;
        }
    }
    else
    { // If it is the last entry, it won't affect the rest so just delete it
        ret = nvs_delete(&storage.nvs, entry_id);
        if (ret < 0)
        {
            LOG_WRN("Failed to delete %s at index %d", key, entry_id);
            return ret;
        }
    }

    counter--;
    ret = nvs_write(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter));
    if (ret < 0)
    {
        LOG_WRN("Failed to decrement counter to %d (%d)", counter, ret);
        return ret;
    }

    return 0;
}

static int settings_clear(void)
{
    int ret = nvs_clear(&storage.nvs);
    if (!ret)
    {
        storage.initialized = false;
        ret = init_flash(); // We need to reinitialize the flash
        if (!ret)
        {
            storage.initialized = true;
        }
    }
    return 0;
}

// Work queue handler and submission

static void setting_work_handler(struct k_work *work)
{
    struct akira_setting_work *sw = CONTAINER_OF(work, struct akira_setting_work, work);
    int result = -1;

    k_mutex_lock(&akira_settings_mutex, K_FOREVER);

    switch (sw->type)
    {
    case AKIRA_SETTINGS_OP_SET:
    {
        if (sw->key && sw->value)
        {
            result = settings_set(sw->key, sw->value, sw->encrypted);
            if (result == 0)
            {
                atomic_inc(&akira_settings_generation);
            }
        }
        break;
    }
    case AKIRA_SETTINGS_OP_GET:
    {
        if (sw->key && sw->value)
        {
            result = settings_get(sw->key, sw->value, sw->max_len);
        }
        break;
    }
    case AKIRA_SETTINGS_OP_DELETE:
    {
        if (sw->key)
        {
            result = settings_delete(sw->key);
            if (result == 0)
            {
                atomic_inc(&akira_settings_generation);
            }
        }
        break;
    }
    case AKIRA_SETTINGS_OP_CLEAR:
    {
        result = settings_clear();
        if (result == 0)
        {
            atomic_inc(&akira_settings_generation);
        }
        break;
    }
    default:
    {
        LOG_WRN("PUSHED UNKNOWN TYPE TO WORKQUEUE (%d)", sw->type);
        break;
    }
    }

    k_mutex_unlock(&akira_settings_mutex);

    if (sw->callback)
    {
        sw->callback(result, sw->user_data);
    }

    if (sw->completion_sem)
    {
        if (sw->result_ptr)
        {
            *sw->result_ptr = result;
        }
        k_sem_give(sw->completion_sem);
    }

    if (sw->callback)
    {
        if (sw->key)
            k_free(sw->key);
        if (sw->type == AKIRA_SETTINGS_OP_SET && sw->value)
        {
            k_free(sw->value);
        }
    }

    k_free(sw);
}

static int submit_settings_work(struct akira_setting_work *work)
{
    if (!storage.initialized)
    {
        if (k_sem_take(&storage_ready_sem, K_MSEC(SETTINGS_INIT_WAIT_MS)) == 0)
        {
            k_sem_give(&storage_ready_sem); /* re-arm for any other waiter */
        }
    }

    if (!storage.initialized)
    {
        if (work->callback)
        {
            if (work->key)
                k_free(work->key);
            if (work->type == AKIRA_SETTINGS_OP_SET && work->value)
                k_free(work->value);
        }
        k_free(work);
        return -EINVAL;
    }

    k_work_init(&work->work, setting_work_handler);
    k_work_submit_to_queue(&storage.work_queue, &work->work);

    return 0;
}

/*===========================================================================*/
/* Public Api                                                                */
/*===========================================================================*/

int akira_settings_init(void)
{
    if (storage.initialized)
    {
        return 0;
    }

    int ret = crypto_init();
    if (ret != 0)
    {
        LOG_ERR("Failed to initialize encryption: %d", ret);
        return ret;
    }

    ret = init_flash();

    if (!ret)
    {
        k_work_queue_init(&storage.work_queue);

        k_work_queue_start(&storage.work_queue,
                           work_stack,
                           K_THREAD_STACK_SIZEOF(work_stack),
                           K_PRIO_PREEMPT(7),
                           NULL);

        storage.initialized = true;
        k_sem_give(&storage_ready_sem);
        LOG_INF("Storage initialized to FLASH");
    }
    else
    {
        LOG_WRN("Storage initialization failed!");
    }
    return ret;
}

int akira_settings_set(const char *key, const char *value, uint8_t is_encrypted)
{
    if (!key || !value)
    {
        return -EINVAL;
    }

    if (strlen(value) >= MAX_VALUE_LEN)
    {
        LOG_ERR("Value too long: %zu >= %d", strlen(value), MAX_VALUE_LEN);
        return -E2BIG;
    }

    if (strlen(key) >= MAX_KEY_LEN)
    {
        LOG_ERR("Key too long: %zu >= %d", strlen(key), MAX_KEY_LEN);
        return -E2BIG;
    }

    struct k_sem completion_sem;
    k_sem_init(&completion_sem, 0, 1);

    int result = -1;
    struct akira_setting_work *work = k_malloc(sizeof(struct akira_setting_work));
    if (!work)
        return -ENOMEM;

    work->type = AKIRA_SETTINGS_OP_SET;
    work->key = (char *)key;
    work->value = (char *)value;
    work->encrypted = is_encrypted;
    work->callback = NULL;
    work->user_data = NULL;
    work->completion_sem = &completion_sem;
    work->result_ptr = &result;

    int ret = submit_settings_work(work);
    if (ret != 0)
    {
        return ret;
    }

    k_sem_take(&completion_sem, K_FOREVER);

    if (result == 0)
    {
        LOG_DBG("Set: %s = %s", key, value);
    }
    return result;
}

unsigned int akira_settings_get_generation(void)
{
    return (unsigned int)atomic_get(&akira_settings_generation);
}

int akira_settings_get(const char *key, char *value, size_t max_len)
{
    if (!key || !value || max_len == 0 || !storage.initialized)
    {
        return -EINVAL;
    }

    if (strlen(key) >= MAX_KEY_LEN)
    {
        LOG_ERR("Key too long: %zu >= %d", strlen(key), MAX_KEY_LEN);
        return -EINVAL;
    }

    struct k_sem completion_sem;
    k_sem_init(&completion_sem, 0, 1);

    int result = -1;
    struct akira_setting_work *work = k_malloc(sizeof(struct akira_setting_work));
    if (!work)
        return -ENOMEM;

    work->type = AKIRA_SETTINGS_OP_GET;
    work->key = (char *)key;
    work->value = value;
    work->max_len = max_len;
    work->callback = NULL;
    work->user_data = NULL;
    work->completion_sem = &completion_sem;
    work->result_ptr = &result;

    int ret = submit_settings_work(work);
    if (ret != 0)
    {
        return ret;
    }

    k_sem_take(&completion_sem, K_FOREVER);

    if (result == 0)
    {
        LOG_DBG("GET: %s = %s", key, value);
    }
    return result;
}

int akira_settings_delete(const char *key)
{
    if (!key || !storage.initialized)
    {
        return -EINVAL;
    }

    struct k_sem completion_sem;
    k_sem_init(&completion_sem, 0, 1);

    int result = -1;
    struct akira_setting_work *work = k_malloc(sizeof(struct akira_setting_work));
    if (!work)
        return -ENOMEM;

    work->type = AKIRA_SETTINGS_OP_DELETE;
    work->key = (char *)key;
    work->value = NULL;
    work->callback = NULL;
    work->user_data = NULL;
    work->completion_sem = &completion_sem;
    work->result_ptr = &result;

    int ret = submit_settings_work(work);
    if (ret != 0)
    {
        return ret;
    }

    k_sem_take(&completion_sem, K_FOREVER);

    if (result == 0)
    {
        LOG_DBG("Deleted: %s", key);
    }
    return result;
}

int akira_settings_clear(void)
{
    if (!storage.initialized)
    {
        return -EINVAL;
    }

    struct k_sem completion_sem;
    k_sem_init(&completion_sem, 0, 1);

    int result = -1;
    struct akira_setting_work *work = k_malloc(sizeof(struct akira_setting_work));
    if (!work)
        return -ENOMEM;

    work->type = AKIRA_SETTINGS_OP_CLEAR;
    work->key = NULL;
    work->value = NULL;
    work->callback = NULL;
    work->user_data = NULL;
    work->completion_sem = &completion_sem;
    work->result_ptr = &result;

    int ret = submit_settings_work(work);
    if (ret != 0)
    {
        return ret;
    }

    k_sem_take(&completion_sem, K_FOREVER);

    if (result == 0)
    {
        LOG_DBG("Cleared");
    }
    return result;
}

int akira_settings_list(settings_iterator_t *iter)
{
    if (!iter || !storage.initialized)
    {
        return -EINVAL;
    }
    int ret = -1;

    if (iter->count == 0)
    { // Initiliaze iterator
        uint16_t counter;
        ret = nvs_read(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter));
        if (ret < 0)
        {
            LOG_WRN("Failed to read SETTINGS_COUNTER_ID (%d)", ret);
            return ret;
        }
        iter->count = counter;
        iter->index = 0;
    }

    if (iter->index >= iter->count)
    {
        return 1; // Indicate end of iteration
    }

    settings_entry_t entry;
    int entry_id = SETTINGS_START_ID + iter->index;
    ret = nvs_read(&storage.nvs, entry_id, &entry, sizeof(entry));
    if (ret < 0)
    {
        LOG_WRN("Failed to read at index %d", entry_id);
        return ret;
    }

    strncpy(iter->key, entry.key, MAX_KEY_LEN - 1);
    iter->key[MAX_KEY_LEN - 1] = '\0';

    if (entry.encrypted)
    {
#ifdef CONFIG_AKIRA_SETTINGS_ENCRYPTION
        uint8_t decoded[MAX_VALUE_LEN];
        size_t decoded_len;
        ret = base64_decode(decoded, sizeof(decoded), &decoded_len,
                            entry.value, strlen(entry.value));

        if (ret == 0)
        {
            ret = crypto_decrypt(decoded, decoded_len, iter->value, MAX_VALUE_LEN);
            if (ret < 0)
            {
                LOG_WRN("Failed to decrypt value at index %d", entry_id);
                strncpy(iter->value, "[ENCRYPTED]", MAX_VALUE_LEN - 1);
            }
        }
        else
        {
            strncpy(iter->value, "[ENCRYPTED]", MAX_VALUE_LEN - 1);
        }
#else
        strncpy(iter->value, "[ENCRYPTED]", MAX_VALUE_LEN - 1);
#endif
    }
    else
    {
        strncpy(iter->value, entry.value, MAX_VALUE_LEN - 1);
    }

    iter->value[MAX_VALUE_LEN - 1] = '\0';
    ++(iter->index);
    return 0;
}

int akira_settings_set_async(const char *key, const char *value, settings_wq_callback_t callback, void *user_data, uint8_t is_encrypted)
{
    if (!key || !value || !storage.initialized)
    {
        return -EINVAL;
    }
    if (!callback)
    {
        LOG_INF("Callback cannot be NULL for async operations");
        return -EINVAL;
    }
    if (strlen(value) >= MAX_VALUE_LEN)
    {
        LOG_ERR("Value too long: %zu >= %d", strlen(value), MAX_VALUE_LEN);
        return -E2BIG;
    }

    if (strlen(key) >= MAX_KEY_LEN)
    {
        LOG_ERR("Key too long: %zu >= %d", strlen(key), MAX_KEY_LEN);
        return -E2BIG;
    }

    struct akira_setting_work *work = k_malloc(sizeof(struct akira_setting_work));
    if (!work)
        return -ENOMEM;

    work->type = AKIRA_SETTINGS_OP_SET;
    work->key = k_malloc(strlen(key) + 1);
    work->value = k_malloc(strlen(value) + 1);
    work->encrypted = is_encrypted;
    work->callback = callback;
    work->user_data = user_data;
    work->completion_sem = NULL;
    work->result_ptr = NULL;

    if (!work->key || !work->value)
    {
        if (work->key)
            k_free(work->key);
        if (work->value)
            k_free(work->value);
        k_free(work);
        return -ENOMEM;
    }

    strcpy(work->key, key);
    strcpy(work->value, value);

    return submit_settings_work(work);
}

int akira_settings_delete_async(const char *key, settings_wq_callback_t callback, void *user_data)
{
    if (!key || !storage.initialized)
    {
        return -EINVAL;
    }

    struct akira_setting_work *work = k_malloc(sizeof(struct akira_setting_work));
    if (!work)
        return -ENOMEM;

    work->type = AKIRA_SETTINGS_OP_DELETE;
    work->key = k_malloc(strlen(key) + 1);
    work->value = NULL;
    work->callback = callback;
    work->user_data = user_data;
    work->completion_sem = NULL;
    work->result_ptr = NULL;

    if (!work->key)
    {
        k_free(work);
        return -ENOMEM;
    }

    strcpy(work->key, key);

    return submit_settings_work(work);
}

/*===========================================================================*/
/* Shell Functions                                                           */
/*===========================================================================*/

static int cmd_settings_get(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_error(sh, "Usage: settings get <key>");
        shell_print(sh, "");
        shell_print(sh, "Description:");
        shell_print(sh, "  Retrieves the value associated with a key.");
        shell_print(sh, "  Automatically decrypts encrypted values.");
        shell_print(sh, "");
        shell_print(sh, "Examples:");
        shell_print(sh, "  settings get user/name");
        shell_print(sh, "  settings get config/timeout");
        shell_print(sh, "  settings get device/id");
        shell_print(sh, "  settings get user/password    # Auto-decrypts if encrypted");
        return -EINVAL;
    }

    char value[MAX_VALUE_LEN];

    int ret = akira_settings_get(argv[1], value, sizeof(value));

    if (ret == 0)
    {
        shell_print(sh, "%s = %s", argv[1], value);
    }
    else if (ret == -ENOENT)
    {
        shell_error(sh, "Key not found: %s", argv[1]);
    }
    else
    {
        shell_error(sh, "Error: %d", ret);
    }

    return ret;
}

static int cmd_settings_set(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 3)
    {
        shell_error(sh, "Usage: settings set [-e] <key> <value>");
        shell_print(sh, "");
        shell_print(sh, "Description:");
        shell_print(sh, "  Stores a key-value pair. Creates the key if it doesn't exist.");
        shell_print(sh, "  Use -e flag to encrypt sensitive data (passwords, tokens, etc.)");
        shell_print(sh, "");
        shell_print(sh, "Options:");
        shell_print(sh, "  -e    Encrypt the value using AES-256-GCM");
        shell_print(sh, "");
        shell_print(sh, "Examples:");
        shell_print(sh, "  settings set user/name \"John Doe\"");
        shell_print(sh, "  settings set config/timeout 30");
        shell_print(sh, "  settings set device/id ABC123");
        shell_print(sh, "  settings set -e user/password \"MySecret123\"");
        shell_print(sh, "  settings set -e api/token \"sk-abc123xyz\"");
        shell_print(sh, "");
        shell_print(sh, "Note:");
        shell_print(sh, "  - Encrypted values are transparently decrypted when retrieved");
        shell_print(sh, "  - Encryption requires CONFIG_AKIRA_SETTINGS_ENCRYPTION=y");
        return -EINVAL;
    }

    bool encrypt = false;
    const char *key;
    const char *value;

    if (strcmp(argv[1], "-e") == 0)
    {
        if (argc < 4)
        {
            shell_error(sh, "Usage: settings set -e <key> <value>");
            return -EINVAL;
        }
        encrypt = true;
        key = argv[2];
        value = argv[3];
    }
    else
    {
        encrypt = false;
        key = argv[1];
        value = argv[2];
    }

    int ret;
    if (encrypt)
    {
#ifdef CONFIG_AKIRA_SETTINGS_ENCRYPTION
        ret = akira_settings_set(key, value, true);
        if (!ret)
        {
            shell_print(sh, "✅ Set (encrypted) %s = [ENCRYPTED]", key);
        }
        else
        {
            shell_error(sh, "Failed to encrypt: %d", ret);
        }
#else
        shell_error(sh, "❌ Encryption not enabled in build!");
        shell_error(sh, "Enable CONFIG_AKIRA_SETTINGS_ENCRYPTION=y to use -e flag");
        ret = -ENOTSUP;
#endif
    }
    else
    {
        ret = akira_settings_set(key, value, false);
        if (!ret)
        {
            shell_print(sh, "✅ Set %s = %s", key, value);
        }
        else
        {
            shell_error(sh, "Failed: %d", ret);
        }
    }

    return ret;
}

static int cmd_settings_list(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "");
    shell_print(sh, "Description:");
    shell_print(sh, "  Lists all stored key-value pairs.");
    shell_print(sh, "  Encrypted values are automatically decrypted for display.");
    shell_print(sh, "");

    char key_buf[MAX_KEY_LEN];
    char value_buf[MAX_VALUE_LEN];

    settings_iterator_t iter = {
        .index = 0,
        .count = 0,
        .key = key_buf,
        .value = value_buf,
    };

    int count = 0;

    shell_print(sh, "Stored keys and values:");
    shell_print(sh, "───────────────────────────────────────────────────────");

    while (akira_settings_list(&iter) == 0)
    {
        shell_print(sh, "%s = %s", iter.key, iter.value);
        count++;
    }

    shell_print(sh, "───────────────────────────────────────────────────────");
    shell_print(sh, "Total: %d keys", count);
    shell_print(sh, "");

    return 0;
}

static int cmd_settings_delete(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_error(sh, "Usage: settings delete <key>");
        shell_print(sh, "");
        shell_print(sh, "Description:");
        shell_print(sh, "  Permanently deletes a key-value pair from storage.");
        shell_print(sh, "");
        shell_print(sh, "Examples:");
        shell_print(sh, "  settings delete user/name");
        shell_print(sh, "  settings delete config/timeout");
        shell_print(sh, "  settings delete device/id");
        shell_print(sh, "  settings delete user/password");
        shell_print(sh, "");
        shell_print(sh, "Note: This operation cannot be undone");
        return -EINVAL;
    }

    int ret = akira_settings_delete(argv[1]);

    if (ret == 0)
    {
        shell_print(sh, "✅ Deleted %s", argv[1]);
    }
    else if (ret == -ENOENT)
    {
        shell_error(sh, "Key not found: %s", argv[1]);
    }
    else
    {
        shell_error(sh, "Failed: %d", ret);
    }

    return ret;
}

static int cmd_settings_clear(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "confirm") != 0)
    {
        shell_error(sh, "Usage: settings clear confirm");
        shell_print(sh, "");
        shell_print(sh, "Description:");
        shell_print(sh, "  Erases ALL stored settings from flash.");
        shell_print(sh, "  This removes both encrypted and plaintext data.");
        shell_print(sh, "");
        shell_warn(sh, "⚠️  WARNING: This will DELETE ALL stored data!");
        shell_warn(sh, "⚠️  This action CANNOT be undone!");
        shell_warn(sh, "⚠️  All keys - values pairs will be erased!");
        shell_print(sh, "");
        shell_print(sh, "To proceed, type:");
        shell_print(sh, "  settings clear confirm");
        shell_print(sh, "");
        return 0;
    }

    int ret = akira_settings_clear();

    if (ret == 0)
    {
        shell_print(sh, "✅ All data cleared successfully");
        shell_print(sh, "Storage has been reset to initial state");
    }
    else
    {
        shell_error(sh, "❌ Clear failed: %d", ret);
    }

    return ret;
}

static int cmd_settings_info(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "");
    shell_print(sh, "Description:");
    shell_print(sh, "  Displays current storage configuration and usage statistics.");
    shell_print(sh, "");
    shell_print(sh, "Storage Configuration:");
    shell_print(sh, "───────────────────────────────────────────────────────");
    shell_print(sh, "Storage type:     Flash (NVS)");
    shell_print(sh, "Max value length: %d bytes", MAX_VALUE_LEN);
    shell_print(sh, "Max keys:         %d", MAX_KEYS);

#ifdef CONFIG_AKIRA_SETTINGS_ENCRYPTION
    shell_print(sh, "Encryption:       Enabled (AES-256-GCM)");
#else
    shell_print(sh, "Encryption:       Disabled");
#endif

    uint16_t counter;
    int ret = nvs_read(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter));

    if (ret >= 0)
    {
        shell_print(sh, "Current keys:     %d", counter);
        shell_print(sh, "Available slots:  %d", MAX_KEYS - counter);

        int usage_x10 = (counter * 1000) / MAX_KEYS;
        shell_print(sh, "Usage:            %d.%d%%", usage_x10 / 10, usage_x10 % 10);
    }

    shell_print(sh, "───────────────────────────────────────────────────────");
    shell_print(sh, "");

    return 0;
}

static int cmd_settings_set_wifi(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 3)
    {
        shell_error(sh, "Usage: settings set_wifi <ssid> <psk>");
        shell_print(sh, "");
        shell_print(sh, "Description:");
        shell_print(sh, "  Convenience command to set WiFi credentials.");
        shell_print(sh, "  SSID is stored as plaintext, PSK is encrypted.");
        shell_print(sh, "");
        shell_print(sh, "Examples:");
        shell_print(sh, "  settings set_wifi \"MyNetwork\" \"MyPassword123\"");
        shell_print(sh, "  settings set_wifi HomeWiFi SecurePass456");
        shell_print(sh, "");
        shell_print(sh, "Note:");
        shell_print(sh, "  - SSID is stored at: %s", AKIRA_SETTINGS_WIFI_SSID_KEY);
        shell_print(sh, "  - PSK is stored encrypted at: %s", AKIRA_SETTINGS_WIFI_PSK_KEY);
        return -EINVAL;
    }

    const char *ssid = argv[1];
    const char *psk = argv[2];

    // Set SSID (plaintext)
    int ret = akira_settings_set(AKIRA_SETTINGS_WIFI_SSID_KEY, ssid, false);
    if (ret != 0)
    {
        shell_error(sh, "Failed to set SSID: %d", ret);
        return ret;
    }

    // Set PSK (encrypted)
#ifdef CONFIG_AKIRA_SETTINGS_ENCRYPTION
    ret = akira_settings_set(AKIRA_SETTINGS_WIFI_PSK_KEY, psk, true);
    if (ret != 0)
    {
        shell_error(sh, "Failed to set PSK: %d", ret);
        // Try to delete the SSID we just set since operation failed
        akira_settings_delete(AKIRA_SETTINGS_WIFI_SSID_KEY);
        return ret;
    }
#else
    shell_warn(sh, "⚠️  Encryption not enabled - PSK will be stored in plaintext!");
    ret = akira_settings_set(AKIRA_SETTINGS_WIFI_PSK_KEY, psk, false);
    if (ret != 0)
    {
        shell_error(sh, "Failed to set PSK: %d", ret);
        akira_settings_delete(AKIRA_SETTINGS_WIFI_SSID_KEY);
        return ret;
    }
#endif

    shell_print(sh, "✅ WiFi credentials set successfully");
    shell_print(sh, "   SSID: %s", ssid);
    shell_print(sh, "   PSK:  [ENCRYPTED]");

    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(settings_cmds,
                               SHELL_CMD_ARG(get, NULL, "Get value for a key", cmd_settings_get, 2, 0),
                               SHELL_CMD_ARG(set, NULL, "Set value for a key (use -e to encrypt)", cmd_settings_set, 3, 1),
                               SHELL_CMD_ARG(list, NULL, "List all key-value pairs", cmd_settings_list, 1, 0),
                               SHELL_CMD_ARG(delete, NULL, "Delete a key-value pair", cmd_settings_delete, 2, 0),
                               SHELL_CMD_ARG(set_wifi, NULL, "Set WiFi SSID and PSK (PSK encrypted)", cmd_settings_set_wifi, 3, 0),
                               SHELL_CMD_ARG(clear, NULL, "Clear all stored data (requires confirmation)", cmd_settings_clear, 1, 1),
                               SHELL_CMD_ARG(info, NULL, "Show storage configuration and statistics", cmd_settings_info, 1, 0),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(settings, &settings_cmds, "Akira persistent settings management", NULL);

#ifdef CONFIG_AKIRA_SETTINGS
SYS_INIT(akira_settings_init, APPLICATION, CONFIG_AKIRA_SETTINGS_INIT_PRIORITY);
#endif