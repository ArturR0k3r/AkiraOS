/**
 * @file se050_settings.c
 * @brief SE050-backed NVS encryption key (strong override).
 *
 * Overrides the __weak akira_settings_get_encryption_key() in settings.c so the
 * 32-byte AES-256 key used for encrypted NVS never exists as a compile-time
 * constant. The key is stored in an SE050 secure binary object; on first boot,
 * if the object is absent, it is provisioned from the SE050 hardware TRNG.
 *
 * Gated by CONFIG_AKIRA_SE050_SETTINGS_KEY.
 */

#include "../drivers/secure_element/se050.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(se050_settings, CONFIG_LOG_DEFAULT_LEVEL);

#define KEY_LEN     32
#define KEY_OBJID   CONFIG_AKIRA_SE050_SETTINGS_KEY_OBJID

int akira_settings_get_encryption_key(uint8_t *key_out)
{
    const struct device *se = se050_get_device();
    if (!se) {
        LOG_ERR("SE050 unavailable — cannot obtain encryption key");
        return -ENODEV;
    }

    size_t out_len = 0;
    int ret = se050_read_binary(se, KEY_OBJID, key_out, KEY_LEN, &out_len);
    if (ret == 0 && out_len == KEY_LEN) {
        LOG_INF("NVS encryption key loaded from SE050");
        return 0;
    }
    if (ret != -ENOENT && ret != 0) {
        LOG_ERR("SE050 key read failed: %d", ret);
        return ret;
    }

    /* First boot (object absent, or wrong length): provision from the TRNG. */
    LOG_INF("Provisioning NVS encryption key in SE050 (first boot)");
    ret = se050_get_random(se, key_out, KEY_LEN);
    if (ret < 0) {
        LOG_ERR("SE050 TRNG failed: %d", ret);
        return ret;
    }
    ret = se050_write_binary(se, KEY_OBJID, key_out, KEY_LEN);
    if (ret < 0) {
        LOG_ERR("SE050 key store failed: %d", ret);
        /* Wipe the freshly generated key from the caller's buffer on failure. */
        memset(key_out, 0, KEY_LEN);
        return ret;
    }
    return 0;
}
