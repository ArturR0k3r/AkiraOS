/**
 * @file akira_capability.h
 * @brief Capability registry: named permission bits granted to WASM apps
 *
 * Every capability an app can request in its manifest is a registry entry that
 * maps a name ("display.write", "acme.valve") to one bit of the 64-bit
 * capability mask. AkiraOS core capabilities use bits 0-47 and their numbers are
 * frozen. Product firmware defines its own capabilities in bits 48-63:
 *
 * @code
 * AKIRA_CAPABILITY_DEFINE(acme_valve, "acme.valve", 48, AKIRA_CAPABILITY_PRIVILEGED);
 * @endcode
 *
 * Rules for AKIRA_CAPABILITY_DEFINE():
 * - The bit must be an integer literal (48, not 0x30 or 48U) so that two
 *   definitions of the same bit fail to link.
 * - The name must be a string literal of at most 63 characters, contain a '.',
 *   contain no '*', and must not collide with a core name, alias or wildcard
 *   prefix. The runtime rejects violations at start-up.
 * - Place the definition in sources compiled into the `app` target, or into a
 *   zephyr_library() created by a Zephyr *module*. Zephyr links those
 *   whole-archive, so the entry is kept. A zephyr_library() an application
 *   creates after find_package(Zephyr) is not whole-archived, and an object
 *   holding only registry entries is silently dropped from it.
 *
 * A manifest may request every capability of a product namespace with a
 * wildcard such as "acme.*".
 *
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_CAPABILITY_H
#define AKIRA_CAPABILITY_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Highest bit reserved for AkiraOS core capabilities. */
#define AKIRA_CAP_CORE_LAST_BIT    47
/** First bit available to product (vendor) capabilities. */
#define AKIRA_CAP_VENDOR_FIRST_BIT 48
/** Last bit available to product (vendor) capabilities. */
#define AKIRA_CAP_VENDOR_LAST_BIT  63
/** Longest capability name a manifest can carry. */
#define AKIRA_CAP_NAME_MAX_LEN     63

/**
 * The capability lets an app affect the world outside its sandbox. Granting it
 * to an app the runtime cannot attest is audit-logged, and
 * CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK can refuse it.
 */
#define AKIRA_CAPABILITY_PRIVILEGED BIT(0)

/** One registry entry. Create entries with AKIRA_CAPABILITY_DEFINE(). */
struct akira_capability {
    const char *name;
    uint64_t mask;
    uint8_t bit;
    uint8_t flags;
};

/** @cond INTERNAL_HIDDEN */
#define Z_AKIRA_CAPABILITY_DEFINE(id, cap_name, cap_bit, cap_flags)              \
    extern const char __akira_cap_bit_##cap_bit;                                \
    const char __akira_cap_bit_##cap_bit = 0;                                   \
    extern const char __akira_cap_id_##id;                                      \
    const char __akira_cap_id_##id = 0;                                         \
    const STRUCT_SECTION_ITERABLE(akira_capability, id) = {                     \
        cap_name, BIT64(cap_bit), (uint8_t)(cap_bit), (uint8_t)(cap_flags)      \
    }
/** @endcond */

/**
 * @brief Define a product capability.
 *
 * @param id        C identifier for the entry, unique across the image.
 * @param cap_name  Manifest string, e.g. "acme.valve".
 * @param cap_bit   Integer literal in the range 48-63.
 * @param cap_flags 0 or AKIRA_CAPABILITY_PRIVILEGED.
 */
#define AKIRA_CAPABILITY_DEFINE(id, cap_name, cap_bit, cap_flags)                  \
    BUILD_ASSERT((cap_bit) >= AKIRA_CAP_VENDOR_FIRST_BIT &&                       \
                 (cap_bit) <= AKIRA_CAP_VENDOR_LAST_BIT,                          \
                 "product capability bits must be in the range 48-63");         \
    BUILD_ASSERT(sizeof(cap_name) - 1 <= AKIRA_CAP_NAME_MAX_LEN,                  \
                 "capability name is longer than 63 characters");               \
    Z_AKIRA_CAPABILITY_DEFINE(id, cap_name, cap_bit, cap_flags)

/**
 * @brief Map a manifest capability string to its mask.
 *
 * Accepts registry names, the core aliases and wildcards, "<product>.*" for
 * product namespaces, and "*" for every registered capability.
 *
 * @return The mask, or 0 for NULL, empty or unknown strings.
 */
uint64_t akira_capability_str_to_mask(const char *name);

/**
 * @brief Name of the lowest capability bit set in @p mask.
 *
 * @return The registry name, or "unknown" when no registered bit is set.
 */
const char *akira_capability_name(uint64_t mask);

/** @brief OR of every registered capability. */
uint64_t akira_capability_known_mask(void);

/** @brief OR of every registered capability flagged privileged. */
uint64_t akira_capability_privileged_mask(void);

/**
 * @brief Compute the capabilities to grant an app.
 *
 * Clamps @p requested to registered capabilities and, for an app that is not
 * attested, to CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK (and away from privileged
 * capabilities when CONFIG_AKIRA_HARDEN_UNATTESTED_APPS=y).
 *
 * @param requested          Mask parsed from the app manifest.
 * @param attested           Admission verdict (akira_admission.h): a trusted
 *                           signature covers the binary and its manifest.
 *                           Before 1.7 this was "manifest embedded in binary".
 * @param unattested_privileged Optional output: privileged bits granted to an
 *                           app that was not attested (for audit logging).
 * @return The capability mask to grant.
 */
uint64_t akira_capability_grant(uint64_t requested, bool attested,
                                uint64_t *unattested_privileged);

/**
 * @brief Check a range of registry entries for collisions and rule violations.
 *
 * @return 0 if valid, -EEXIST on duplicate names or bits, -EINVAL on an invalid
 *         bit, mask or product capability name.
 */
int akira_capability_validate(const struct akira_capability *begin,
                              const struct akira_capability *end);

/** @brief Validate every capability linked into the image. */
int akira_capability_registry_validate(void);

#if defined(CONFIG_AKIRA_WASM_RUNTIME) || defined(__DOXYGEN__)
#include <wasm_export.h>

/**
 * @brief True if the app running in @p exec_env holds @p capability.
 *
 * Use at the top of every native function (AKIRA_NATIVE_API_DEFINE). Denials
 * are audited and rate-limited by the sandbox.
 *
 * @since 1.6 (public since 1.7; previously runtime/security.h)
 */
bool akira_security_check_exec(wasm_exec_env_t exec_env, uint64_t capability);

/** Return @p retval from the calling native function unless the app holds
 *  @p capability. */
#define AKIRA_CHECK_CAP_OR_RETURN(exec_env, capability, retval)                    \
    do {                                                                           \
        if (!akira_security_check_exec(exec_env, capability)) {                   \
            return (retval);                                                       \
        }                                                                          \
    } while (0)

/** As AKIRA_CHECK_CAP_OR_RETURN() for native functions returning void. */
#define AKIRA_CHECK_CAP_OR_RETURN_VOID(exec_env, capability)                       \
    do {                                                                           \
        if (!akira_security_check_exec(exec_env, capability)) {                   \
            return;                                                                \
        }                                                                          \
    } while (0)

/** Expression form: true if the app holds @p capability. */
#define AKIRA_CHECK_CAP_INLINE(exec_env, capability)                               \
    akira_security_check_exec(exec_env, capability)
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CAPABILITY_H */
