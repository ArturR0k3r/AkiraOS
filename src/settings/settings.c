#include <akira_keyprovider.h>
#include "settings.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>
#include <lib/mem_helper.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef CONFIG_AKIRA_SETTINGS_ENCRYPTION
#include <mbedtls/gcm.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_util.h>
#include <zephyr/random/random.h>
#ifdef CONFIG_AKIRA_SETTINGS_PER_DEVICE_KEY
#include <mbedtls/sha256.h>
#include <zephyr/drivers/hwinfo.h>
#endif
#endif

LOG_MODULE_REGISTER(akira_settings, CONFIG_LOG_DEFAULT_LEVEL);

#ifdef CONFIG_AKIRA_SETTINGS_ENCRYPTION

#define ENCRYPTION_MAGIC 0xAE01
#define MAGIC_SIZE 2
#define IV_SIZE 12
#define TAG_SIZE 16

/* Thread-context only (settings_set/get encrypt/decrypt before nvs_write,
 * never from ISR, never DMA'd - HW AES accel here is register-poll, no
 * DMA path in the mbedtls port), safe in PSRAM like ENCRYPTION_KEY below. */
static struct
{
    mbedtls_gcm_context gcm;
    bool initialized;
} crypto_ctx AKIRA_BULK_BSS;

static uint8_t ENCRYPTION_KEY[32] AKIRA_BULK_BSS;

static int parse_hex_key_into(uint8_t *out)
{
    const char *hex = CONFIG_AKIRA_SETTINGS_ENCRYPTION_KEY_HEX;

    if (strlen(hex) != 64)
    {
        return -EINVAL;
    }

    for (int i = 0; i < 32; i++)
    {
        out[i] = HEX_TO_BYTE(hex[i * 2], hex[i * 2 + 1]);
    }

    return 0;
}

static int parse_hex_key(void)
{
    return parse_hex_key_into(ENCRYPTION_KEY);
}

/* The encryption key comes from the first settings key provider
 * (include/akira_keyprovider.h) that supplies one; without a provider it is
 * derived from CONFIG_AKIRA_SETTINGS_ENCRYPTION_KEY_HEX (default_settings_key()). */
#ifdef CONFIG_AKIRA_SETTINGS_PER_DEVICE_KEY
/**
 * @brief Derive a per-device AES-256 key = HMAC-SHA256(base_key, device_id).
 *
 * Binds the effective key to a hardware-unique identifier so that the shared
 * compile-time base key alone cannot decrypt any device's settings. Fails
 * closed: if the device id is unavailable, no key is produced.
 */
static int derive_per_device_key(const uint8_t *base_key, uint8_t *key_out)
{
    uint8_t device_id[16];
    ssize_t id_len = hwinfo_get_device_id(device_id, sizeof(device_id));
    if (id_len <= 0)
    {
        LOG_ERR("Per-device key: hwinfo_get_device_id failed (%d) — refusing "
                "to fall back to the shared compile-time key", (int)id_len);
        return -ENODEV;
    }

    /* HMAC-SHA256(key = base_key, msg = device_id), implemented directly on top
     * of mbedtls_sha256 (already linked) so we do not depend on the optional
     * generic MD layer. Block size B=64, output L=32; base_key is 32 bytes so it
     * fits a single block without pre-hashing. */
    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    uint8_t inner[32];
    mbedtls_sha256_context sha;
    int ret;

    memset(k_ipad, 0, sizeof(k_ipad));
    memset(k_opad, 0, sizeof(k_opad));
    memcpy(k_ipad, base_key, 32);
    memcpy(k_opad, base_key, 32);
    for (int i = 0; i < 64; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }

    /* inner = SHA256(k_ipad || device_id) */
    mbedtls_sha256_init(&sha);
    ret = mbedtls_sha256_starts(&sha, 0);
    ret |= mbedtls_sha256_update(&sha, k_ipad, sizeof(k_ipad));
    ret |= mbedtls_sha256_update(&sha, device_id, (size_t)id_len);
    ret |= mbedtls_sha256_finish(&sha, inner);
    mbedtls_sha256_free(&sha);
    if (ret != 0) {
        LOG_ERR("Per-device key: inner HMAC hash failed");
        return -EIO;
    }

    /* key_out = SHA256(k_opad || inner) */
    mbedtls_sha256_init(&sha);
    ret = mbedtls_sha256_starts(&sha, 0);
    ret |= mbedtls_sha256_update(&sha, k_opad, sizeof(k_opad));
    ret |= mbedtls_sha256_update(&sha, inner, sizeof(inner));
    ret |= mbedtls_sha256_finish(&sha, key_out);
    mbedtls_sha256_free(&sha);
    if (ret != 0) {
        LOG_ERR("Per-device key: outer HMAC hash failed");
        return -EIO;
    }

    LOG_INF("Per-device settings key derived from %d-byte hardware ID", (int)id_len);
    return 0;
}
#endif /* CONFIG_AKIRA_SETTINGS_PER_DEVICE_KEY */

static int default_settings_key(uint8_t *key_out)
{
    int ret = parse_hex_key();
    if (ret != 0)
    {
        return ret;
    }
#ifdef CONFIG_AKIRA_SETTINGS_PER_DEVICE_KEY
    /* Mix the compile-time base key with the hardware-unique device id so the
     * effective key differs per unit. Fails closed if the id is unavailable. */
    return derive_per_device_key(ENCRYPTION_KEY, key_out);
#else
    memcpy(key_out, ENCRYPTION_KEY, 32);
    return 0;
#endif
}

#define SETTINGS_KEY_MAX_PROVIDERS 8

static int obtain_settings_key(uint8_t *key_out)
{
    const struct akira_settings_key_provider *order[SETTINGS_KEY_MAX_PROVIDERS];
    int n = 0;

    STRUCT_SECTION_FOREACH(akira_settings_key_provider, p)
    {
        if (!p->fn)
        {
            continue;
        }
        if (n == SETTINGS_KEY_MAX_PROVIDERS)
        {
            LOG_ERR("More than %d settings key providers", SETTINGS_KEY_MAX_PROVIDERS);
            return -E2BIG;
        }
        int i = n++;
        while (i > 0 && order[i - 1]->prio > p->prio)
        {
            order[i] = order[i - 1];
            i--;
        }
        order[i] = p;
    }

    for (int i = 0; i < n; i++)
    {
        int ret = order[i]->fn(key_out, order[i]->user_data);
        if (ret == 0)
        {
            LOG_INF("Settings key from provider '%s'", order[i]->name);
            return 0;
        }
        if (ret != -ENOTSUP && ret != -ENODEV)
        {
            /* Fail closed: a provider that exists but fails must not fall
             * back to a weaker key (and data encrypted under its key would
             * be unreadable with another one anyway). */
            LOG_ERR("Settings key provider '%s' failed: %d", order[i]->name, ret);
            return ret;
        }
    }

    return default_settings_key(key_out);
}

static int crypto_init(void)
{
    if (crypto_ctx.initialized)
    {
        return 0;
    }

    int ret = obtain_settings_key(ENCRYPTION_KEY);
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

/* Queue that drains settings requests. A dedicated thread by default; boards
 * short on internal DRAM (AkiraConsole) run them on the system work queue
 * instead, saving that thread's 2 KB stack, which needs a system work queue
 * stack big enough for NVS + AES-GCM (CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE). */
#ifdef CONFIG_AKIRA_SETTINGS_ON_SYSWORKQ
#define SETTINGS_WORK_Q (&k_sys_work_q)
#else
K_THREAD_STACK_DEFINE(work_stack, 2048);
static struct k_work_q settings_work_q;
#define SETTINGS_WORK_Q (&settings_work_q)
#endif

/* Longest app name kept with an app request (akira_managed_app_t.name). */
#define SETTINGS_APP_NAME_MAX 32

/*
 * One queued settings operation. Requests are plain heap blocks in a list that
 * one static work item (s_pump) drains, so a request can be freed as soon as
 * it is done: a k_work freed by its own handler is still written by the work
 * queue after the handler returns.
 *
 * A request owns copies of its key and value. A synchronous caller holds a
 * reference, waits on @done and copies a GET result out; whoever drops the
 * last reference frees the request. Nothing in it points at the caller's
 * stack or buffers, so a caller that never comes back (an app thread aborted
 * while it waited) leaves nothing behind for the handler to write into.
 */
struct akira_setting_work
{
    sys_snode_t node;
    settings_op_type_t type;
    uint16_t max_len;
    uint8_t encrypted;
    bool is_app;    /* key lives in app's namespace (akira_settings_app_*) */
    bool counted;   /* counted in s_app_pending */

    settings_wq_callback_t callback;
    void *user_data;

    atomic_t refs;
    struct k_sem done;
    int result;

    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
    char app[SETTINGS_APP_NAME_MAX];
};

static sys_slist_t s_req_list;
static struct k_spinlock s_req_lock;

/* App requests queued or running: bounds what apps that give up waiting
 * can leave in the queue. */
#define APP_PENDING_MAX 16
static atomic_t s_app_pending;

static struct
{
    struct nvs_fs nvs;
    bool initialized;
} storage AKIRA_BULK_BSS;

/* read_entry() result: the record was written by a build with a different
 * CONFIG_AKIRA_SETTINGS_MAX_VALUE_LEN and was converted to this build's layout. */
#define ENTRY_RESIZED 1

/*
 * Read settings entry @id into @e.
 *
 * A record is key[MAX_KEY_LEN], value[], encrypted (one byte, last). Builds
 * with a different CONFIG_AKIRA_SETTINGS_MAX_VALUE_LEN (stock 1.7: 64, the
 * AkiraConsole: 192) write shorter or longer records, and reading one as
 * settings_entry_t would take the encrypted flag from the wrong byte (or from
 * stack garbage past a short record). Normalise it instead.
 *
 * @return 0, ENTRY_RESIZED, or a negative errno (-ENOENT for a hole).
 */
static int read_entry(uint16_t id, settings_entry_t *e)
{
    memset(e, 0, sizeof(*e));

    ssize_t rc = nvs_read(&storage.nvs, id, e, sizeof(*e));
    if (rc < 0)
    {
        return (int)rc;
    }
    if ((size_t)rc == sizeof(*e))
    {
        e->key[MAX_KEY_LEN - 1] = '\0';
        e->value[MAX_VALUE_LEN - 1] = '\0';
        return 0;
    }
    if ((size_t)rc < MAX_KEY_LEN + 2)
    {
        return -EBADMSG;
    }

    uint8_t *raw = (uint8_t *)e;
    uint8_t encrypted;

    if ((size_t)rc < sizeof(*e))
    {
        /* key and value are in place; the flag landed inside value[]. */
        encrypted = raw[rc - 1];
        memset(raw + rc - 1, 0, sizeof(*e) - (size_t)(rc - 1));
    }
    else
    {
        uint8_t *full = akira_malloc_buffer((size_t)rc);
        if (!full)
        {
            return -ENOMEM;
        }
        ssize_t rc2 = nvs_read(&storage.nvs, id, full, (size_t)rc);
        if (rc2 != rc)
        {
            akira_free_buffer(full);
            return rc2 < 0 ? (int)rc2 : -EIO;
        }
        size_t stored_len = strnlen((const char *)full + MAX_KEY_LEN,
                                    (size_t)rc - MAX_KEY_LEN - 1);
        encrypted = full[rc - 1];
        memset(e, 0, sizeof(*e));
        memcpy(e->key, full, MAX_KEY_LEN);
        memcpy(e->value, full + MAX_KEY_LEN, MIN(stored_len, (size_t)MAX_VALUE_LEN - 1));
        akira_free_buffer(full);
        if (stored_len >= MAX_VALUE_LEN)
        {
            LOG_WRN("Setting '%.*s' is longer than this build's %d-byte values; truncated",
                    MAX_KEY_LEN - 1, e->key, MAX_VALUE_LEN);
        }
    }

    e->key[MAX_KEY_LEN - 1] = '\0';
    e->encrypted = encrypted ? 1 : 0;
    return ENTRY_RESIZED;
}

/* Compact NVS entries: reads all valid entries, rewrites them sequentially
 * from SETTINGS_START_ID, and updates the counter.  Called at init when
 * holes are detected (e.g. after OTA that wiped the old NVS partition address
 * but left a stale counter value). */
static int compact_entries(uint16_t counter)
{
    /* Heap-allocate to avoid blowing the stack (MAX_KEYS * ~97 bytes).
     * PSRAM-first: this only runs at init when NVS holes are detected, and
     * the whole sequence already blocks on NVS flash I/O under a mutex, so
     * PSRAM's extra access latency is noise here — not a hot loop. */
    settings_entry_t *buf = akira_malloc_buffer(sizeof(settings_entry_t) * counter);
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
        int r = read_entry(SETTINGS_START_ID + i, &buf[valid]);
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
    akira_free_buffer(buf);

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
        ret = read_entry(SETTINGS_START_ID + i, &entry);
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
            if (read_entry(SETTINGS_START_ID + i, &probe) < 0)
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
        uint8_t *encrypted_buf = akira_malloc_buffer(encrypted_buf_size);
        if (!encrypted_buf)
        {
            LOG_ERR("Failed to allocate encryption buffer");
            return -ENOMEM;
        }

        int encrypted_len = crypto_encrypt(value, encrypted_buf, encrypted_buf_size);
        if (encrypted_len < 0)
        {
            LOG_ERR("Encryption failed: %d", encrypted_len);
            akira_free_buffer(encrypted_buf);
            return encrypted_len;
        }

        size_t b64_len = 4 * ((encrypted_len + 2) / 3) + 1;
        b64_value = akira_malloc_buffer(b64_len);
        if (!b64_value)
        {
            akira_free_buffer(encrypted_buf);
            return -ENOMEM;
        }

        size_t written;
        int ret = base64_encode(b64_value, b64_len, &written, encrypted_buf, encrypted_len);
        akira_free_buffer(encrypted_buf);

        if (ret != 0)
        {
            akira_free_buffer(b64_value);
            return ret;
        }

        if (strlen(b64_value) >= MAX_VALUE_LEN)
        {
            LOG_ERR("Encrypted value too long: %zu >= %d", strlen(b64_value), MAX_VALUE_LEN);
            akira_free_buffer(b64_value);
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
            akira_free_buffer(b64_value);
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
                akira_free_buffer(b64_value);
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
                akira_free_buffer(b64_value);
            return ret;
        }
    }
    if (b64_value)
        akira_free_buffer(b64_value);
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
    ret = read_entry(entry_id, &entry);
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
        ret = read_entry(last_entry_id, &last_entry);
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

// Request queue: one static work item drains s_req_list

/* Length of @p s, at most @p max, reading no byte past the NUL (strnlen()
 * is not in every libc here; @p s may sit at the end of an app's memory). */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n] != '\0')
    {
        n++;
    }
    return n;
}

/* Apps' requests come from PSRAM only on boards that have it (an app can
 * make that fail); the OS's may fall back to internal SRAM. */
static struct akira_setting_work *request_new(settings_op_type_t type, bool for_app)
{
    struct akira_setting_work *sw = for_app ? akira_malloc_app_buffer(sizeof(*sw))
                                            : akira_malloc_buffer(sizeof(*sw));

    if (sw)
    {
        memset(sw, 0, sizeof(*sw));
        sw->type = type;
        atomic_set(&sw->refs, 1);
        k_sem_init(&sw->done, 0, 1);
    }
    return sw;
}

static void request_put(struct akira_setting_work *sw)
{
    if (atomic_dec(&sw->refs) == 1)
    {
        akira_free_buffer(sw);
    }
}

static int app_request(struct akira_setting_work *sw);

static void run_request(struct akira_setting_work *sw)
{
    int result = -1;

    k_mutex_lock(&akira_settings_mutex, K_FOREVER);

    if (sw->is_app)
    {
        result = app_request(sw);
    }
    else
    {
        switch (sw->type)
        {
        case AKIRA_SETTINGS_OP_SET:
            result = settings_set(sw->key, sw->value, sw->encrypted);
            break;
        case AKIRA_SETTINGS_OP_GET:
            result = settings_get(sw->key, sw->value, sw->max_len);
            break;
        case AKIRA_SETTINGS_OP_DELETE:
            result = settings_delete(sw->key);
            break;
        case AKIRA_SETTINGS_OP_CLEAR:
            result = settings_clear();
            break;
        default:
            LOG_WRN("PUSHED UNKNOWN TYPE TO WORKQUEUE (%d)", sw->type);
            break;
        }
    }

    /* Every successful mutation, OS or app, bumps the generation. */
    if (result == 0 && sw->type != AKIRA_SETTINGS_OP_GET)
    {
        atomic_inc(&akira_settings_generation);
    }

    k_mutex_unlock(&akira_settings_mutex);

    if (sw->counted)
    {
        atomic_dec(&s_app_pending);
    }
    if (sw->callback)
    {
        sw->callback(result, sw->user_data);
    }
    sw->result = result;
    k_sem_give(&sw->done);
    request_put(sw);
}

static void pump_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    for (;;)
    {
        k_spinlock_key_t key = k_spin_lock(&s_req_lock);
        sys_snode_t *node = sys_slist_get(&s_req_list);

        k_spin_unlock(&s_req_lock, key);
        if (!node)
        {
            return;
        }
        run_request(CONTAINER_OF(node, struct akira_setting_work, node));
    }
}

static K_WORK_DEFINE(s_pump, pump_handler);

/* Queue @p work. On failure it is freed, whatever references it had. */
static int submit_settings_work(struct akira_setting_work *work)
{
    if (!storage.initialized)
    {
        /* Storage init may still be running (the shell can take a command
         * before it finishes): wait for it rather than fail outright. */
        if (k_sem_take(&storage_ready_sem, K_MSEC(SETTINGS_INIT_WAIT_MS)) == 0)
        {
            k_sem_give(&storage_ready_sem); /* re-arm for any other waiter */
        }
    }

    if (!storage.initialized)
    {
        akira_free_buffer(work);
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&s_req_lock);

    sys_slist_append(&s_req_list, &work->node);
    k_spin_unlock(&s_req_lock, key);
    /* Already queued: nothing to do; running: it runs once more. */
    (void)k_work_submit_to_queue(SETTINGS_WORK_Q, &s_pump);

    return 0;
}

/* Copy a GET result out of a finished request. */
static void request_copy_value(const struct akira_setting_work *sw, char *out, size_t out_len)
{
    size_t n = bounded_len(sw->value, MIN(out_len - 1, sizeof(sw->value) - 1));

    memcpy(out, sw->value, n);
    out[n] = '\0';
}

/* Queue @p sw and wait for it, as long as it takes (OS callers). */
static int run_sync(struct akira_setting_work *sw, char *out, size_t out_len)
{
    int ret;

    atomic_set(&sw->refs, 2);

    if (k_current_get() == k_work_queue_thread_get(SETTINGS_WORK_Q))
    {
        /* A caller already running on the queue that drains the requests
         * (a callback, or any system work item when that is the system work
         * queue) would wait for itself forever. Run the request here. */
        if (!storage.initialized)
        {
            akira_free_buffer(sw);
            return -EINVAL;
        }
        run_request(sw);
    }
    else
    {
        ret = submit_settings_work(sw);
        if (ret != 0)
        {
            return ret;
        }
        (void)k_sem_take(&sw->done, K_FOREVER);
    }
    ret = sw->result;
    if (out && ret == 0)
    {
        request_copy_value(sw, out, out_len);
    }
    request_put(sw);
    return ret;
}

/*===========================================================================*/
/* WASM app settings                                                         */
/*===========================================================================*/

/*
 * Each app's keys live in a namespace of their own, "a/<ns>/<key>", where
 * <ns> is two hex digits bound to the app's exact name by a record
 * "a/n/<ns>" = <app name>. An app can't name a key outside its namespace, so
 * the OS's keys (Wi-Fi, MQTT, mesh identity, boot guard...) and other apps'
 * keys are out of reach, and no hash collision can share a namespace.
 *
 * New keys are limited per app, for all apps together and by the
 * partition's key budget (MAX_KEYS), so apps can't fill the partition the
 * OS's settings live in. Writes are rate-limited by the caller
 * (akira_settings_api.c), per app.
 */
#define APP_NS_NAME_PREFIX "a/n/"
#define APP_NS_MAX         64   /* namespaces 00..3f */
#define APP_NS_PREFIX_LEN  5    /* "a/<ns>/" */
#define APP_KEYS_PER_APP   40
#define APP_KEYS_ALL_APPS  64   /* names included */
#define APP_WAIT_SLICE_MS  50

#define NS_BIT_TEST(map, ns) (((map)[(ns) / 8] & BIT((ns) % 8)) != 0)
#define NS_BIT_SET(map, ns)  ((map)[(ns) / 8] |= (uint8_t)BIT((ns) % 8))

struct app_ns_scan
{
    int ns;                          /* the app's namespace, or -1 */
    int all;                         /* entries under "a/": names and keys */
    int total;                       /* entries in the partition */
    uint8_t keys[APP_NS_MAX];        /* keys per namespace */
    uint8_t named[APP_NS_MAX / 8];   /* namespaces bound to a name */
    uint8_t has_key[APP_NS_MAX / 8]; /* namespaces holding the key asked for */
};

/* Two lowercase hex digits, or -1. */
static int ns_parse(const char *p)
{
    int v = 0;

    for (int i = 0; i < 2; i++)
    {
        char c = p[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;

        if (d < 0)
        {
            return -1;
        }
        v = v * 16 + d;
    }
    return v;
}

static const char ns_hex[] = "0123456789abcdef";

/* out (MAX_KEY_LEN bytes) = "a/<ns>/<key>"; callers checked the key fits. */
static void app_ns_key(char *out, int ns, const char *key)
{
    size_t n = bounded_len(key, MAX_KEY_LEN - APP_NS_PREFIX_LEN - 1);

    out[0] = 'a';
    out[1] = '/';
    out[2] = ns_hex[(ns >> 4) & 0xf];
    out[3] = ns_hex[ns & 0xf];
    out[4] = '/';
    memcpy(out + APP_NS_PREFIX_LEN, key, n);
    out[APP_NS_PREFIX_LEN + n] = '\0';
}

/* out (MAX_KEY_LEN bytes) = "a/n/<ns>" */
static void app_ns_name(char *out, int ns)
{
    memcpy(out, APP_NS_NAME_PREFIX, 4);
    out[4] = ns_hex[(ns >> 4) & 0xf];
    out[5] = ns_hex[ns & 0xf];
    out[6] = '\0';
}

/* One pass over the partition: the app's namespace, the counts the quotas
 * need, and which namespaces hold @p key. Caller holds the mutex. */
static int app_ns_scan(const char *app, const char *key, struct app_ns_scan *s)
{
    uint16_t counter;
    settings_entry_t entry;
    int ret = nvs_read(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter));

    memset(s, 0, sizeof(*s));
    s->ns = -1;
    if (ret < 0)
    {
        return ret;
    }

    for (uint16_t i = 0; i < counter; i++)
    {
        memset(&entry, 0, sizeof(entry));
        ret = read_entry(SETTINGS_START_ID + i, &entry);
        if (ret == -ENOENT)
        {
            continue; /* hole */
        }
        if (ret < 0)
        {
            return ret;
        }
        s->total++;
        entry.key[sizeof(entry.key) - 1] = '\0';
        entry.value[sizeof(entry.value) - 1] = '\0';
        if (entry.key[0] != 'a' || entry.key[1] != '/')
        {
            continue;
        }

        int ns;

        if (strncmp(entry.key, APP_NS_NAME_PREFIX, 4) == 0)
        {
            ns = ns_parse(entry.key + 4);
            s->all++;
            if (ns >= 0 && ns < APP_NS_MAX && entry.key[6] == '\0')
            {
                NS_BIT_SET(s->named, ns);
                if (s->ns < 0 && strcmp(entry.value, app) == 0)
                {
                    s->ns = ns;
                }
            }
        }
        else if ((ns = ns_parse(entry.key + 2)) >= 0 && entry.key[4] == '/')
        {
            s->all++;
            if (ns < APP_NS_MAX)
            {
                if (s->keys[ns] < UINT8_MAX)
                {
                    s->keys[ns]++;
                }
                if (key && strcmp(entry.key + APP_NS_PREFIX_LEN, key) == 0)
                {
                    NS_BIT_SET(s->has_key, ns);
                }
            }
        }
    }
    return 0;
}

static int app_set(struct akira_setting_work *sw, const struct app_ns_scan *scan)
{
    char full[MAX_KEY_LEN];
    int ns = scan->ns;
    bool named_now = false;
    int ret;

    if (ns < 0 || !NS_BIT_TEST(scan->has_key, ns))
    {
        /* A new key; a new app also needs its name record. */
        int need = (ns < 0) ? 2 : 1;

        if ((ns >= 0 && scan->keys[ns] >= APP_KEYS_PER_APP) ||
            scan->all + need > APP_KEYS_ALL_APPS ||
            scan->total + need > MAX_KEYS)
        {
            return -ENOSPC;
        }
        if (ns < 0)
        {
            for (ns = 0; ns < APP_NS_MAX; ns++)
            {
                if (!NS_BIT_TEST(scan->named, ns) && scan->keys[ns] == 0)
                {
                    break;
                }
            }
            if (ns == APP_NS_MAX)
            {
                return -ENOSPC;
            }
            app_ns_name(full, ns);
            ret = settings_set(full, sw->app, 0);
            if (ret != 0)
            {
                return ret;
            }
            named_now = true;
        }
    }

    app_ns_key(full, ns, sw->key);
    ret = settings_set(full, sw->value, 0);
    if (ret != 0 && named_now)
    {
        app_ns_name(full, ns);
        (void)settings_delete(full);
    }
    return ret;
}

/* Delete every key of namespace @p ns, then its name. */
static int app_purge(int ns)
{
    uint16_t counter;
    settings_entry_t entry;
    char prefix[MAX_KEY_LEN];
    int ret;

    if (ns < 0)
    {
        return 0;
    }
    app_ns_key(prefix, ns, "");
    ret = nvs_read(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter));
    if (ret < 0)
    {
        return ret;
    }

    /* Top down: settings_delete() moves the last entry into the hole, and
     * every entry above has been looked at already. */
    for (int i = (int)counter - 1; i >= 0; i--)
    {
        memset(&entry, 0, sizeof(entry));
        if (read_entry(SETTINGS_START_ID + i, &entry) < 0)
        {
            continue;
        }
        entry.key[sizeof(entry.key) - 1] = '\0';
        if (strncmp(entry.key, prefix, APP_NS_PREFIX_LEN) == 0)
        {
            ret = settings_delete(entry.key);
            if (ret != 0)
            {
                return ret;
            }
        }
    }

    app_ns_name(entry.key, ns);
    ret = settings_delete(entry.key);
    return (ret == -1) ? 0 : ret;
}

/* Runs on the settings work queue, mutex held. */
static int app_request(struct akira_setting_work *sw)
{
    struct app_ns_scan scan;
    char full[MAX_KEY_LEN];
    int ret;

    ret = app_ns_scan(sw->app, sw->key[0] ? sw->key : NULL, &scan);
    if (ret < 0)
    {
        return ret;
    }
    int ns = scan.ns;

    switch (sw->type)
    {
    case AKIRA_SETTINGS_OP_GET:
        if (ns < 0 || !NS_BIT_TEST(scan.has_key, ns))
        {
            return -ENOENT;
        }
        app_ns_key(full, ns, sw->key);
        ret = settings_get(full, sw->value, sw->max_len);
        return (ret == -1) ? -ENOENT : ret;

    case AKIRA_SETTINGS_OP_DELETE:
        if (ns < 0 || !NS_BIT_TEST(scan.has_key, ns))
        {
            return -ENOENT;
        }
        app_ns_key(full, ns, sw->key);
        ret = settings_delete(full);
        if (ret == 0 && scan.keys[ns] <= 1)
        {
            /* That was the app's last key: its name goes too. */
            app_ns_name(full, ns);
            (void)settings_delete(full);
        }
        return (ret == -1) ? -ENOENT : ret;

    case AKIRA_SETTINGS_OP_SET:
        return app_set(sw, &scan);

    case AKIRA_SETTINGS_OP_CLEAR:
        return app_purge(ns);

    default:
        return -EINVAL;
    }
}

/* A request for app @p app with key @p key, or NULL. */
static struct akira_setting_work *app_request_new(settings_op_type_t op, const char *app,
                                                  const char *key)
{
    size_t app_len = bounded_len(app, SETTINGS_APP_NAME_MAX);
    size_t key_len = key ? bounded_len(key, MAX_KEY_LEN) : 0;
    struct akira_setting_work *sw;

    if (app_len == 0 || app_len >= SETTINGS_APP_NAME_MAX ||
        APP_NS_PREFIX_LEN + key_len >= MAX_KEY_LEN)
    {
        return NULL;
    }
    sw = request_new(op, true);
    if (sw)
    {
        sw->is_app = true;
        memcpy(sw->app, app, app_len);
        if (key)
        {
            memcpy(sw->key, key, key_len);
        }
    }
    return sw;
}

int akira_settings_app_call(settings_op_type_t op, const char *app,
                            const char *key, const char *value,
                            char *out, size_t out_len, k_timeout_t timeout,
                            bool (*give_up)(void *ctx), void *ctx)
{
    size_t key_len, value_len = 0;

    if (!app || !app[0] || !key || !key[0])
    {
        return -EINVAL;
    }
    if (op != AKIRA_SETTINGS_OP_GET && op != AKIRA_SETTINGS_OP_SET &&
        op != AKIRA_SETTINGS_OP_DELETE)
    {
        return -EINVAL;
    }
    if (op == AKIRA_SETTINGS_OP_GET && (!out || out_len == 0))
    {
        return -EINVAL;
    }
    if (bounded_len(app, SETTINGS_APP_NAME_MAX) >= SETTINGS_APP_NAME_MAX)
    {
        return -EINVAL;
    }
    key_len = bounded_len(key, MAX_KEY_LEN);
    if (APP_NS_PREFIX_LEN + key_len >= MAX_KEY_LEN)
    {
        return -ENAMETOOLONG;
    }
    if (op == AKIRA_SETTINGS_OP_SET)
    {
        if (!value)
        {
            return -EINVAL;
        }
        value_len = bounded_len(value, MAX_VALUE_LEN);
        if (value_len >= MAX_VALUE_LEN)
        {
            return -E2BIG;
        }
    }
    if (!storage.initialized)
    {
        return -EAGAIN;
    }

    if (atomic_inc(&s_app_pending) >= APP_PENDING_MAX)
    {
        atomic_dec(&s_app_pending);
        return -EBUSY;
    }

    struct akira_setting_work *sw = app_request_new(op, app, key);

    if (!sw)
    {
        atomic_dec(&s_app_pending);
        return -ENOMEM;
    }
    sw->counted = true;
    if (op == AKIRA_SETTINGS_OP_SET)
    {
        memcpy(sw->value, value, value_len);
    }
    sw->max_len = (uint16_t)MIN(out_len, sizeof(sw->value));
    atomic_set(&sw->refs, 2);

    int ret = submit_settings_work(sw);

    if (ret != 0)
    {
        atomic_dec(&s_app_pending);
        return ret;
    }

    /* Wait in slices: a stopping app returns instead of being aborted here,
     * and gives up after @p timeout. A request left behind still finishes
     * and frees itself. */
    k_timepoint_t end = sys_timepoint_calc(timeout);

    ret = -ETIMEDOUT;
    for (;;)
    {
        k_timeout_t left = sys_timepoint_timeout(end);
        k_timeout_t slice = K_MSEC(APP_WAIT_SLICE_MS);

        if (!K_TIMEOUT_EQ(left, K_FOREVER) && left.ticks < slice.ticks)
        {
            slice = left;
        }
        if (k_sem_take(&sw->done, slice) == 0)
        {
            ret = sw->result;
            break;
        }
        if (give_up && give_up(ctx))
        {
            ret = -ECANCELED;
            break;
        }
        if (sys_timepoint_expired(end))
        {
            break;
        }
    }

    if (op == AKIRA_SETTINGS_OP_GET && ret == 0)
    {
        request_copy_value(sw, out, out_len);
    }
    request_put(sw);
    return ret;
}

int akira_settings_app_purge(const char *app)
{
    if (!app || !app[0] || !storage.initialized ||
        bounded_len(app, SETTINGS_APP_NAME_MAX) >= SETTINGS_APP_NAME_MAX)
    {
        return -EINVAL;
    }

    struct akira_setting_work *sw = app_request_new(AKIRA_SETTINGS_OP_CLEAR, app, NULL);

    if (!sw)
    {
        return -ENOMEM;
    }
    return run_sync(sw, NULL, 0);
}

/*===========================================================================*/
/* Public Api                                                                */
/*===========================================================================*/

#ifdef CONFIG_AKIRA_SETTINGS_ENCRYPTION
/* Scratch for migrate_entries(), heap-allocated: two GCM contexts' worth of
 * state does not belong on the init thread's stack. */
struct settings_migration
{
    settings_entry_t entry;
    uint8_t decoded[MAX_VALUE_LEN];
    char plain[MAX_VALUE_LEN];
    char reencoded[MAX_VALUE_LEN];
#ifdef CONFIG_AKIRA_SETTINGS_LEGACY_KEY_MIGRATION
    uint8_t legacy_key[32];
    mbedtls_gcm_context legacy;
#endif
};

/* Decrypt an encrypted entry's value with @gcm into @out. Quiet: a failure
 * here is expected while probing which key an entry was written with. */
static int entry_decrypt_with(mbedtls_gcm_context *gcm, const settings_entry_t *e,
                              uint8_t *decoded, char *out, size_t out_len)
{
    size_t dlen = 0;

    if (base64_decode(decoded, MAX_VALUE_LEN, &dlen, (const uint8_t *)e->value,
                      strnlen(e->value, MAX_VALUE_LEN)) != 0 ||
        dlen < MAGIC_SIZE + IV_SIZE + TAG_SIZE)
    {
        return -EBADMSG;
    }

    size_t data_len = dlen - MAGIC_SIZE - IV_SIZE - TAG_SIZE;
    if (data_len >= out_len)
    {
        return -E2BIG;
    }

    if (mbedtls_gcm_auth_decrypt(gcm, data_len,
                                 decoded + MAGIC_SIZE, IV_SIZE, NULL, 0,
                                 decoded + MAGIC_SIZE + IV_SIZE + data_len, TAG_SIZE,
                                 decoded + MAGIC_SIZE + IV_SIZE,
                                 (unsigned char *)out) != 0)
    {
        return -EACCES;
    }
    out[data_len] = '\0';
    return 0;
}

/* Encrypt @plain with the current key into a base64 value. */
static int entry_encrypt_b64(const char *plain, char *out, size_t out_len)
{
    size_t enc_size = MAGIC_SIZE + IV_SIZE + strlen(plain) + TAG_SIZE;
    uint8_t *enc = akira_malloc_buffer(enc_size);
    if (!enc)
    {
        return -ENOMEM;
    }

    int enc_len = crypto_encrypt(plain, enc, enc_size);
    if (enc_len < 0)
    {
        akira_free_buffer(enc);
        return enc_len;
    }

    size_t written = 0;
    int ret = base64_encode((uint8_t *)out, out_len, &written, enc, (size_t)enc_len);
    akira_free_buffer(enc);
    return ret == -ENOMEM ? -E2BIG : ret;
}

/*
 * One pass over every entry at boot:
 *  - records written with another CONFIG_AKIRA_SETTINGS_MAX_VALUE_LEN are
 *    rewritten in this build's layout;
 *  - with CONFIG_AKIRA_SETTINGS_LEGACY_KEY_MIGRATION, encrypted values that the
 *    current key can't open but the plain compile-time key can (written before
 *    the per-device key, AkiraOS < 1.7) are re-encrypted with the current key.
 * Entries are rewritten in place (same id), so the counter is untouched.
 */
static void migrate_entries(void)
{
    uint16_t counter;

    if (nvs_read(&storage.nvs, SETTINGS_COUNTER_ID, &counter, sizeof(counter)) < 0 ||
        counter == 0)
    {
        return;
    }

    struct settings_migration *m = akira_malloc_buffer(sizeof(*m));
    if (!m)
    {
        LOG_WRN("Settings migration skipped: out of memory");
        return;
    }
    memset(m, 0, sizeof(*m));

#ifdef CONFIG_AKIRA_SETTINGS_LEGACY_KEY_MIGRATION
    /* Only worth trying when the current key isn't the plain key itself. */
    bool have_legacy = parse_hex_key_into(m->legacy_key) == 0 &&
                       memcmp(m->legacy_key, ENCRYPTION_KEY, sizeof(m->legacy_key)) != 0;
    if (have_legacy)
    {
        mbedtls_gcm_init(&m->legacy);
        have_legacy = mbedtls_gcm_setkey(&m->legacy, MBEDTLS_CIPHER_ID_AES,
                                         m->legacy_key, 256) == 0;
    }
#endif

    int resized = 0;
    int rekeyed = 0;
    int unreadable = 0;

    k_mutex_lock(&akira_settings_mutex, K_FOREVER);

    for (uint16_t i = 0; i < counter; i++)
    {
        uint16_t id = SETTINGS_START_ID + i;
        int r = read_entry(id, &m->entry);
        if (r < 0)
        {
            continue;
        }

        bool rewrite = (r == ENTRY_RESIZED);
        if (rewrite)
        {
            resized++;
        }

        if (m->entry.encrypted &&
            entry_decrypt_with(&crypto_ctx.gcm, &m->entry, m->decoded,
                               m->plain, sizeof(m->plain)) != 0)
        {
            int lr = -EACCES;
#ifdef CONFIG_AKIRA_SETTINGS_LEGACY_KEY_MIGRATION
            if (have_legacy)
            {
                lr = entry_decrypt_with(&m->legacy, &m->entry, m->decoded,
                                        m->plain, sizeof(m->plain));
            }
#endif
            if (lr == 0 &&
                entry_encrypt_b64(m->plain, m->reencoded, sizeof(m->reencoded)) == 0)
            {
                memcpy(m->entry.value, m->reencoded, sizeof(m->entry.value));
                m->entry.value[MAX_VALUE_LEN - 1] = '\0';
                rewrite = true;
                rekeyed++;
            }
            else
            {
                unreadable++;
                LOG_WRN("Setting '%s' can't be decrypted with this device's key; set it again",
                        m->entry.key);
            }
        }

        if (rewrite && nvs_write(&storage.nvs, id, &m->entry, sizeof(m->entry)) < 0)
        {
            LOG_WRN("Settings migration: rewriting '%s' failed", m->entry.key);
        }
        mbedtls_platform_zeroize(m->plain, sizeof(m->plain));
    }

    k_mutex_unlock(&akira_settings_mutex);

#ifdef CONFIG_AKIRA_SETTINGS_LEGACY_KEY_MIGRATION
    if (have_legacy)
    {
        mbedtls_gcm_free(&m->legacy);
    }
#endif
    mbedtls_platform_zeroize(m, sizeof(*m));
    akira_free_buffer(m);

    if (resized || rekeyed || unreadable)
    {
        LOG_INF("Settings migration: %d resized, %d re-encrypted with this device's key, "
                "%d unreadable", resized, rekeyed, unreadable);
    }
}
#endif /* CONFIG_AKIRA_SETTINGS_ENCRYPTION */

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
#ifdef CONFIG_AKIRA_SETTINGS_ENCRYPTION
        migrate_entries();
#endif
#ifndef CONFIG_AKIRA_SETTINGS_ON_SYSWORKQ
        k_work_queue_init(&settings_work_q);
        k_work_queue_start(&settings_work_q,
                           work_stack,
                           K_THREAD_STACK_SIZEOF(work_stack),
                           K_PRIO_PREEMPT(7),
                           NULL);
#endif
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

    struct akira_setting_work *work = request_new(AKIRA_SETTINGS_OP_SET, false);
    if (!work)
        return -ENOMEM;

    strcpy(work->key, key);
    strcpy(work->value, value);
    work->encrypted = is_encrypted;

    int result = run_sync(work, NULL, 0);

    if (result == 0)
    {
        /* Key only: values may be secrets. */
        LOG_DBG("Set: %s", key);
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

    struct akira_setting_work *work = request_new(AKIRA_SETTINGS_OP_GET, false);
    if (!work)
        return -ENOMEM;

    strcpy(work->key, key);
    /* Stored values (and their decryptions) are shorter than MAX_VALUE_LEN,
     * so capping the length changes no result. */
    work->max_len = (uint16_t)MIN(max_len, sizeof(work->value));

    int result = run_sync(work, value, max_len);

    if (result == 0)
    {
        LOG_DBG("GET: %s", key);
    }
    return result;
}

int akira_settings_delete(const char *key)
{
    if (!key || !storage.initialized)
    {
        return -EINVAL;
    }
    if (strlen(key) >= MAX_KEY_LEN)
    {
        return -1; /* no stored key is this long: not found, as before */
    }

    struct akira_setting_work *work = request_new(AKIRA_SETTINGS_OP_DELETE, false);
    if (!work)
        return -ENOMEM;

    strcpy(work->key, key);

    int result = run_sync(work, NULL, 0);

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

    struct akira_setting_work *work = request_new(AKIRA_SETTINGS_OP_CLEAR, false);
    if (!work)
        return -ENOMEM;

    int result = run_sync(work, NULL, 0);

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
    ret = read_entry(entry_id, &entry);
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

    struct akira_setting_work *work = request_new(AKIRA_SETTINGS_OP_SET, false);
    if (!work)
        return -ENOMEM;

    strcpy(work->key, key);
    strcpy(work->value, value);
    work->encrypted = is_encrypted;
    work->callback = callback;
    work->user_data = user_data;

    return submit_settings_work(work);
}

int akira_settings_delete_async(const char *key, settings_wq_callback_t callback, void *user_data)
{
    if (!key || !storage.initialized)
    {
        return -EINVAL;
    }
    if (strlen(key) >= MAX_KEY_LEN)
    {
        return -E2BIG;
    }

    struct akira_setting_work *work = request_new(AKIRA_SETTINGS_OP_DELETE, false);
    if (!work)
        return -ENOMEM;

    strcpy(work->key, key);
    work->callback = callback;
    work->user_data = user_data;

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
        shell_print(sh, "  Stored as one encrypted entry, written atomically.");
        shell_print(sh, "");
        shell_print(sh, "Examples:");
        shell_print(sh, "  settings set_wifi \"MyNetwork\" \"MyPassword123\"");
        shell_print(sh, "  settings set_wifi HomeWiFi SecurePass456");
        shell_print(sh, "");
        shell_print(sh, "Note:");
        shell_print(sh, "  - Stored at: %s", AKIRA_SETTINGS_WIFI_CREDS_KEY);
        return -EINVAL;
    }

    const char *ssid = argv[1];
    const char *psk = argv[2];

    /* SSID+PSK as one tab-delimited value — a single NVS write is atomic,
     * unlike two separate writes which leave a window where a power loss
     * between them strands a new SSID paired with the old PSK. */
    char combined[MAX_VALUE_LEN * 2 + 2];
    int n = snprintf(combined, sizeof(combined), "%s\t%s", ssid, psk);
    if (n < 0 || (size_t)n >= sizeof(combined))
    {
        shell_error(sh, "SSID/PSK too long");
        return -ENAMETOOLONG;
    }

#ifndef CONFIG_AKIRA_SETTINGS_ENCRYPTION
    shell_warn(sh, "⚠️  Encryption not enabled - credentials will be stored in plaintext!");
#endif

    int ret = akira_settings_set(AKIRA_SETTINGS_WIFI_CREDS_KEY, combined, true);
    if (ret != 0)
    {
        shell_error(sh, "Failed to set WiFi credentials: %d", ret);
        return ret;
    }

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