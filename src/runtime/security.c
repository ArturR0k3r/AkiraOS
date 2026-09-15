#include "security.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/math_extras.h>
#include <zephyr/sys/util.h>
#include <runtime/security/sandbox.h>

LOG_MODULE_REGISTER(akira_security, CONFIG_AKIRA_LOG_LEVEL);

#include <errno.h>
#include <string.h>
#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

/* ===== Core capability registry =====
 *
 * One entry per AKIRA_CAP_* bit in security.h. Bit numbers are frozen: manifests
 * name capabilities by string, but raw masks also appear in configuration
 * (CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK), logs and the cloud protocol. Names are
 * the canonical strings reported by akira_capability_name(). The privileged flag
 * follows AKIRA_CAP_PRIVILEGED, so the two can never disagree. */
#define AKIRA_CORE_CAPABILITY(id, cap_name, cap_bit, cap_define)                    \
    BUILD_ASSERT(BIT64(cap_bit) == (cap_define),                                \
                 cap_name ": bit number does not match its AKIRA_CAP_ define");  \
    BUILD_ASSERT((cap_bit) <= AKIRA_CAP_CORE_LAST_BIT,                          \
                 cap_name ": core capability bits are 0-47");                   \
    Z_AKIRA_CAPABILITY_DEFINE(id, cap_name, cap_bit,                             \
        (((cap_define) & AKIRA_CAP_PRIVILEGED) ? AKIRA_CAPABILITY_PRIVILEGED : 0))

AKIRA_CORE_CAPABILITY(akira_cap_display_write, "display.write", 0,  AKIRA_CAP_DISPLAY_WRITE);
AKIRA_CORE_CAPABILITY(akira_cap_input_read,    "input.read",    1,  AKIRA_CAP_INPUT_READ);
AKIRA_CORE_CAPABILITY(akira_cap_input_write,   "input.write",   2,  AKIRA_CAP_INPUT_WRITE);
AKIRA_CORE_CAPABILITY(akira_cap_sensor_read,   "sensor.read",   3,  AKIRA_CAP_SENSOR_READ);
AKIRA_CORE_CAPABILITY(akira_cap_rf_transceive, "rf.transceive", 4,  AKIRA_CAP_RF_TRANSCEIVE);
AKIRA_CORE_CAPABILITY(akira_cap_ble,           "ble",           5,  AKIRA_CAP_BLE);
AKIRA_CORE_CAPABILITY(akira_cap_storage_read,  "storage.read",  6,  AKIRA_CAP_STORAGE_READ);
AKIRA_CORE_CAPABILITY(akira_cap_storage_write, "storage.write", 7,  AKIRA_CAP_STORAGE_WRITE);
AKIRA_CORE_CAPABILITY(akira_cap_network,       "network.use",   8,  AKIRA_CAP_NETWORK);
AKIRA_CORE_CAPABILITY(akira_cap_gpio_read,     "gpio.read",     9,  AKIRA_CAP_GPIO_READ);
AKIRA_CORE_CAPABILITY(akira_cap_gpio_write,    "gpio.write",    10, AKIRA_CAP_GPIO_WRITE);
AKIRA_CORE_CAPABILITY(akira_cap_timer,         "timer",         11, AKIRA_CAP_TIMER);
AKIRA_CORE_CAPABILITY(akira_cap_uart,          "uart",          12, AKIRA_CAP_UART);
AKIRA_CORE_CAPABILITY(akira_cap_i2c,           "i2c",           13, AKIRA_CAP_I2C);
AKIRA_CORE_CAPABILITY(akira_cap_pwm,           "pwm",           14, AKIRA_CAP_PWM);
AKIRA_CORE_CAPABILITY(akira_cap_hid,           "hid",           15, AKIRA_CAP_HID);
AKIRA_CORE_CAPABILITY(akira_cap_app_control,   "app.control",   16, AKIRA_CAP_APP_CONTROL);
AKIRA_CORE_CAPABILITY(akira_cap_ipc,           "ipc",           17, AKIRA_CAP_IPC);
AKIRA_CORE_CAPABILITY(akira_cap_app_switch,    "app.switch",    18, AKIRA_CAP_APP_SWITCH);
AKIRA_CORE_CAPABILITY(akira_cap_memory,        "memory",        19, AKIRA_CAP_MEMORY);
AKIRA_CORE_CAPABILITY(akira_cap_app_info,      "app.info",      20, AKIRA_CAP_APP_INFO);
AKIRA_CORE_CAPABILITY(akira_cap_power_read,    "power.read",    21, AKIRA_CAP_POWER_READ);
AKIRA_CORE_CAPABILITY(akira_cap_power_control, "power.control", 22, AKIRA_CAP_POWER_CTRL);
AKIRA_CORE_CAPABILITY(akira_cap_settings,      "settings.*",    23, AKIRA_CAP_SETTINGS);
AKIRA_CORE_CAPABILITY(akira_cap_adc,           "adc",           24, AKIRA_CAP_ADC);
AKIRA_CORE_CAPABILITY(akira_cap_wdt,           "wdt",           25, AKIRA_CAP_WDT);
AKIRA_CORE_CAPABILITY(akira_cap_fs_read,       "fs.read",       26, AKIRA_CAP_FS_READ);
AKIRA_CORE_CAPABILITY(akira_cap_fs_write,      "fs.write",      27, AKIRA_CAP_FS_WRITE);
AKIRA_CORE_CAPABILITY(akira_cap_crypto,        "crypto",        28, AKIRA_CAP_CRYPTO);
AKIRA_CORE_CAPABILITY(akira_cap_rtc_read,      "rtc.read",      29, AKIRA_CAP_RTC_READ);
AKIRA_CORE_CAPABILITY(akira_cap_rtc_write,     "rtc.write",     30, AKIRA_CAP_RTC_WRITE);
AKIRA_CORE_CAPABILITY(akira_cap_ota_trigger,   "ota.trigger",   31, AKIRA_CAP_OTA_TRIGGER);
AKIRA_CORE_CAPABILITY(akira_cap_aiinfer,       "ai.infer",      32, AKIRA_CAP_AIINFER);
AKIRA_CORE_CAPABILITY(akira_cap_matter,        "matter",        33, AKIRA_CAP_MATTER);
AKIRA_CORE_CAPABILITY(akira_cap_wifi_inject,   "wifi.inject",   34, AKIRA_CAP_WIFI_INJECT);
AKIRA_CORE_CAPABILITY(akira_cap_mqtt,          "mqtt",          35, AKIRA_CAP_MQTT);
AKIRA_CORE_CAPABILITY(akira_cap_ble_scan,      "ble.scan",      36, AKIRA_CAP_BLE_SCAN);
AKIRA_CORE_CAPABILITY(akira_cap_ble_spam,      "ble.spam",      37, AKIRA_CAP_BLE_SPAM);
AKIRA_CORE_CAPABILITY(akira_cap_mesh,          "mesh",          38, AKIRA_CAP_MESH);
AKIRA_CORE_CAPABILITY(akira_cap_sync,          "sync",          39, AKIRA_CAP_SYNC);

/* Core manifest strings that are not registry names: legacy spellings,
 * wildcards and composites. Each maps to exactly the mask it has always
 * produced. Product capabilities may not reuse these names or wildcard prefixes. */
struct akira_capability_alias {
    const char *name;
    uint64_t mask;
};

static const struct akira_capability_alias core_aliases[] = {
    { "display.read",    AKIRA_CAP_DISPLAY_WRITE },
    { "bt.shell",        AKIRA_CAP_BLE },
    { "memory.alloc",    AKIRA_CAP_MEMORY },
    { "settings.read",   AKIRA_CAP_SETTINGS },
    { "settings.write",  AKIRA_CAP_SETTINGS },
    { "network.connect", AKIRA_CAP_NETWORK },
    { "display.*",       AKIRA_CAP_DISPLAY_WRITE },
    { "input.*",         AKIRA_CAP_INPUT_READ | AKIRA_CAP_INPUT_WRITE },
    { "sensor.*",        AKIRA_CAP_SENSOR_READ },
    { "rf.*",            AKIRA_CAP_RF_TRANSCEIVE },
    { "bt.*",            AKIRA_CAP_BLE | AKIRA_CAP_HID | AKIRA_CAP_BLE_SCAN | AKIRA_CAP_BLE_SPAM },
    { "storage.*",       AKIRA_CAP_STORAGE_READ | AKIRA_CAP_STORAGE_WRITE },
    { "gpio.*",          AKIRA_CAP_GPIO_READ | AKIRA_CAP_GPIO_WRITE },
    { "network.*",       AKIRA_CAP_NETWORK },
    { "power.*",         AKIRA_CAP_POWER_READ | AKIRA_CAP_POWER_CTRL },
    { "fs.*",            AKIRA_CAP_FS_READ | AKIRA_CAP_FS_WRITE },
    { "rtc.*",           AKIRA_CAP_RTC_READ | AKIRA_CAP_RTC_WRITE },
    { "matter.*",        AKIRA_CAP_MATTER },
    { "mqtt.*",          AKIRA_CAP_MQTT },
    { "mesh.*",          AKIRA_CAP_MESH },
    { "sync.*",          AKIRA_CAP_SYNC },
    { "hw.*",            AKIRA_CAP_TIMER | AKIRA_CAP_UART | AKIRA_CAP_I2C | AKIRA_CAP_PWM },
};

uint64_t akira_capability_known_mask(void)
{
    uint64_t mask = 0;

    STRUCT_SECTION_FOREACH(akira_capability, cap) {
        mask |= cap->mask;
    }
    return mask;
}

uint64_t akira_capability_privileged_mask(void)
{
    uint64_t mask = 0;

    STRUCT_SECTION_FOREACH(akira_capability, cap) {
        if (cap->flags & AKIRA_CAPABILITY_PRIVILEGED) {
            mask |= cap->mask;
        }
    }
    return mask;
}

/* "<prefix>.*" for a product namespace: every product capability named
 * <prefix> or <prefix>.<anything>. Never matches core capabilities. */
static uint64_t vendor_wildcard_mask(const char *name, size_t prefix_len)
{
    uint64_t mask = 0;

    STRUCT_SECTION_FOREACH(akira_capability, cap) {
        if (cap->bit < AKIRA_CAP_VENDOR_FIRST_BIT) {
            continue;
        }
        if (strncmp(cap->name, name, prefix_len) == 0 &&
            (cap->name[prefix_len] == '\0' || cap->name[prefix_len] == '.')) {
            mask |= cap->mask;
        }
    }
    return mask;
}

uint64_t akira_capability_str_to_mask(const char *cap)
{
    if (!cap || cap[0] == '\0') {
        return 0;
    }

    /* Wildcard-all is bounded to the registered capabilities — never
     * UINT64_MAX, which would also set unassigned bits. */
    if (strcmp(cap, "*") == 0) {
        return akira_capability_known_mask();
    }

    for (size_t i = 0; i < ARRAY_SIZE(core_aliases); i++) {
        if (strcmp(cap, core_aliases[i].name) == 0) {
            return core_aliases[i].mask;
        }
    }

    STRUCT_SECTION_FOREACH(akira_capability, entry) {
        if (strcmp(cap, entry->name) == 0) {
            return entry->mask;
        }
    }

    size_t len = strlen(cap);

    if (len > 2 && cap[len - 2] == '.' && cap[len - 1] == '*') {
        return vendor_wildcard_mask(cap, len - 2);
    }
    return 0;
}

uint64_t akira_capability_sanitize_app_mask(uint64_t requested, bool attested)
{
    /* Never honor bits the runtime does not define. */
    uint64_t effective = requested & akira_capability_known_mask();

    if (!attested) {
        /* Unsigned/unattested app: clamp to the operator-configured allow-mask.
         * Default is permissive (all known caps) to preserve local development
         * and on-device security tooling; a production board can narrow
         * CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK to refuse privileged caps to code
         * it cannot attest. */
        effective &= (uint64_t)CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK;
    }

    return effective;
}

uint64_t akira_capability_grant(uint64_t requested, bool from_binary,
                                uint64_t *unattested_privileged)
{
    /* Only a manifest embedded in the binary can be covered by the platform
     * signature gate; a separately supplied JSON manifest never is. It is
     * re-read from the (unauthenticated) filesystem at start, after any install-
     * time signature check, so it cannot be trusted to raise privilege. On a
     * hardened board (CONFIG_AKIRA_REQUIRE_SIGNED_APPS=y with a narrowed
     * CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK) a signed app must therefore embed its
     * manifest in the WASM binary (AkiraSDK embed_manifest.py) for its
     * capabilities to be attested; a sidecar-only manifest is clamped. */
    bool attested = from_binary && IS_ENABLED(CONFIG_AKIRA_REQUIRE_SIGNED_APPS);
    uint64_t granted = akira_capability_sanitize_app_mask(requested, attested);

    if (unattested_privileged) {
        *unattested_privileged =
            attested ? 0 : (granted & akira_capability_privileged_mask());
    }
    return granted;
}

const char *akira_capability_name(uint64_t mask)
{
    if (mask == 0) {
        return "unknown";
    }

    uint8_t bit = (uint8_t)u64_count_trailing_zeros(mask);

    STRUCT_SECTION_FOREACH(akira_capability, cap) {
        if (cap->bit == bit) {
            return cap->name;
        }
    }
    return "unknown";
}

char *akira_capability_mask_to_str(uint64_t cap)
{
    return (char *)akira_capability_name(cap);
}

/* A product capability name must not shadow a core alias or live under a core
 * wildcard prefix ("fs.*" reserves "fs."). */
static bool name_reserved_by_core_alias(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(core_aliases); i++) {
        const char *alias = core_aliases[i].name;
        size_t alias_len = strlen(alias);

        if (strcmp(name, alias) == 0) {
            return true;
        }
        if (alias_len >= 2 && alias[alias_len - 1] == '*' &&
            strncmp(name, alias, alias_len - 1) == 0) {
            return true;
        }
    }
    return false;
}

int akira_capability_validate(const struct akira_capability *begin,
                              const struct akira_capability *end)
{
    int ret = 0;

    for (const struct akira_capability *cap = begin; cap < end; cap++) {
        if (!cap->name || cap->bit > AKIRA_CAP_VENDOR_LAST_BIT ||
            cap->mask != BIT64(cap->bit)) {
            LOG_ERR("Capability %s: invalid bit %u or mask 0x%016llx",
                    cap->name ? cap->name : "(null)", cap->bit,
                    (unsigned long long)cap->mask);
            ret = -EINVAL;
            continue;
        }

        if (cap->bit >= AKIRA_CAP_VENDOR_FIRST_BIT) {
            size_t len = strlen(cap->name);

            if (len == 0 || len > AKIRA_CAP_NAME_MAX_LEN ||
                strchr(cap->name, '*') != NULL || strchr(cap->name, '.') == NULL ||
                name_reserved_by_core_alias(cap->name)) {
                LOG_ERR("Product capability \"%s\" (bit %u): name must be 1-%d "
                        "characters, contain '.', contain no '*', and not reuse a "
                        "core alias or wildcard prefix",
                        cap->name, cap->bit, AKIRA_CAP_NAME_MAX_LEN);
                ret = -EINVAL;
            }
        }

        for (const struct akira_capability *other = begin; other < cap; other++) {
            if (!other->name) {
                continue;
            }
            if (other->bit == cap->bit || strcmp(other->name, cap->name) == 0) {
                LOG_ERR("Capabilities \"%s\" (bit %u) and \"%s\" (bit %u) collide",
                        other->name, other->bit, cap->name, cap->bit);
                ret = -EEXIST;
            }
        }
    }
    return ret;
}

int akira_capability_registry_validate(void)
{
    STRUCT_SECTION_START_EXTERN(akira_capability);
    STRUCT_SECTION_END_EXTERN(akira_capability);

    return akira_capability_validate(STRUCT_SECTION_START(akira_capability),
                                     STRUCT_SECTION_END(akira_capability));
}

/* Convenience wrapper for native callers */
bool akira_security_check(uint64_t capability)
{
    return akira_security_check_native(capability);
}

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* Get capability mask for exec_env - for use with inline macros */
uint64_t akira_security_get_cap_mask(wasm_exec_env_t exec_env)
{
    if (!exec_env) return 0;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    return akira_runtime_get_cap_mask_for_module_inst(inst);
}

bool akira_security_check_exec(wasm_exec_env_t exec_env, uint64_t capability)
{
    if (!exec_env) return false;

    uint64_t mask = akira_security_get_cap_mask(exec_env);

    bool ok = (mask & capability) != 0;
    if (!ok) {
        wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
        char namebuf[32];
        if (akira_runtime_get_name_for_module_inst(inst, namebuf, sizeof(namebuf)) == 0) {
            LOG_WRN("Security: capability denied for app %s: %s (0x%016llx)", namebuf, akira_capability_name(capability), (unsigned long long)capability);
            sandbox_audit_log(AUDIT_EVENT_CAPABILITY_DENIED, namebuf, (uint32_t)(capability & 0xFFFFFFFFu));
        } else {
            LOG_WRN("Security: capability denied for unknown app: %s (0x%016llx)", akira_capability_name(capability), (unsigned long long)capability);
            sandbox_audit_log(AUDIT_EVENT_CAPABILITY_DENIED, "unknown", (uint32_t)(capability & 0xFFFFFFFFu));
        }
    }
    return ok;
}
#else
uint64_t akira_security_get_cap_mask(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return 0;
}

bool akira_security_check_exec(wasm_exec_env_t exec_env, uint64_t capability)
{
    (void)exec_env; (void)capability;
    return false;
}
#endif

bool akira_security_check_native(uint64_t capability)
{
    /* This is the TRUSTED-NATIVE path: it grants unconditionally because the
     * only legitimate callers are first-party OS/kernel code that already runs
     * with full privilege. It must NEVER be reachable from a WASM app — every
     * WASM-invocable native API MUST go through akira_security_check_exec()
     * (see the AKIRA_CHECK_CAP_* macros), which enforces the per-app cap mask.
     * Do not call akira_security_check()/_native() from any exec_env handler. */
    (void)capability;
    return true;
}
