/**
 * @file akira_keyprovider.h
 * @brief Source of the settings encryption key
 *
 * Encrypted settings (CONFIG_AKIRA_SETTINGS_ENCRYPTION) use one AES-256 key.
 * By default AkiraOS derives it from CONFIG_AKIRA_SETTINGS_ENCRYPTION_KEY_HEX,
 * mixed with the hardware device id (CONFIG_AKIRA_SETTINGS_PER_DEVICE_KEY). A
 * provider replaces that with a key from a secure element or TrustZone:
 *
 * @code
 * static int acme_key(uint8_t key[AKIRA_SETTINGS_KEY_LEN], void *user)
 * {
 *     return acme_se_derive(key);   // -ENOTSUP: let the next provider try
 * }
 * AKIRA_SETTINGS_KEY_PROVIDER_DEFINE(acme_key_provider, 10, acme_key, NULL);
 * @endcode
 *
 * Providers run in ascending @p prio order. The first one to return 0 supplies
 * the key. -ENOTSUP or -ENODEV passes to the next provider; any other error
 * fails settings initialisation (it does not fall back to a weaker key, which
 * would also make previously encrypted values unreadable).
 *
 * The provider is called once, from the settings SYS_INIT
 * (CONFIG_AKIRA_SETTINGS_INIT_PRIORITY, APPLICATION level). Initialise any
 * hardware it needs on first call rather than relying on SYS_INIT ordering.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_KEYPROVIDER_H
#define AKIRA_KEYPROVIDER_H

#include <stdint.h>
#include <zephyr/sys/iterable_sections.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AKIRA_SETTINGS_KEY_LEN 32

typedef int (*akira_settings_key_fn)(uint8_t key_out[AKIRA_SETTINGS_KEY_LEN],
                                     void *user_data);

/** One registered provider. Create with AKIRA_SETTINGS_KEY_PROVIDER_DEFINE(). */
struct akira_settings_key_provider {
    const char *name;
    int16_t prio;
    akira_settings_key_fn fn;
    void *user_data;
};

/** Register a settings key provider. */
#define AKIRA_SETTINGS_KEY_PROVIDER_DEFINE(id, prio, fn, user)                     \
    const STRUCT_SECTION_ITERABLE(akira_settings_key_provider, id) = {             \
        #id, (prio), (fn), (user)                                                  \
    }

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_KEYPROVIDER_H */
