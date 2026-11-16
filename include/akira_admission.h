/**
 * @file akira_admission.h
 * @brief App admission: decide whether an app may be installed or loaded
 *
 * An admission provider sees every app at two points and can refuse it:
 *
 * - AKIRA_ADMISSION_INSTALL — before an app is written to flash or replaces an
 *   installed app. The provider sees the binary hash, the package signatures
 *   (sig.ed25519, sig.mldsa44) and the package digest they cover.
 * - AKIRA_ADMISSION_LOAD — every time an app is loaded into the runtime,
 *   including SD-XIP, cloud and self-test loads. Signatures are gone by then;
 *   the provider sees the hashes and the admission record written at install.
 *
 * A provider returns a verdict and can mark the app attested (a signature it
 * trusts covers the binary and manifest) or clamp its capabilities:
 *
 * @code
 * static int acme_admit(const struct akira_admission_ctx *ctx,
 *                       struct akira_admission_result *res, void *user)
 * {
 *     if (ctx->phase == AKIRA_ADMISSION_INSTALL &&
 *         ctx->source == AKIRA_APP_SOURCE_BLE) {
 *         res->reason = "BLE install disabled";
 *         return AKIRA_ADMISSION_DENY;
 *     }
 *     return AKIRA_ADMISSION_ABSTAIN;
 * }
 * AKIRA_APP_ADMISSION_DEFINE(acme_admission, 50, acme_admit, NULL);
 * @endcode
 *
 * Combination rules (akira_admission_evaluate()):
 * - Providers run in ascending @p prio order (ties: link order).
 * - The first DENY ends evaluation; the app is refused.
 * - `attested` is true if any provider set it and none denied. At LOAD the
 *   core also requires the stored record to match the binary and manifest.
 * - Capability clamps are ANDed.
 * - CONFIG_AKIRA_REQUIRE_SIGNED_APPS=y refuses an app no provider attested.
 *
 * The core registers one provider: the single-key Ed25519 baseline at
 * AKIRA_ADMISSION_PRIO_CORE (CONFIG_AKIRA_APP_PUBKEY), see docs/security.
 *
 * Providers run on the installing or loading thread (HTTP, shell, BLE, an app
 * restart) and must not call back into the app manager.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_ADMISSION_H
#define AKIRA_ADMISSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/iterable_sections.h>
#include <akira_net_policy.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where an app came from. Values match the app manager's app_source_t. */
enum akira_app_source {
    AKIRA_APP_SOURCE_UNKNOWN = 0,
    AKIRA_APP_SOURCE_HTTP,
    AKIRA_APP_SOURCE_BLE,
    AKIRA_APP_SOURCE_USB,
    AKIRA_APP_SOURCE_SD,
    AKIRA_APP_SOURCE_FIRMWARE,
    /** A fleet / device-management client (e.g. AkiraPlatform). */
    AKIRA_APP_SOURCE_FLEET,
};

/** Evaluation point. */
enum akira_admission_phase {
    AKIRA_ADMISSION_INSTALL = 0,
    AKIRA_ADMISSION_LOAD,
};

/** Provider verdicts. */
enum akira_admission_verdict {
    AKIRA_ADMISSION_ABSTAIN = 0, /**< No opinion. */
    AKIRA_ADMISSION_ALLOW,       /**< Allow (does not by itself attest). */
    AKIRA_ADMISSION_DENY,        /**< Refuse the app. */
};

/** Priority of the core Ed25519 baseline provider. Lower runs first. */
#define AKIRA_ADMISSION_PRIO_CORE 100

#define AKIRA_ADMISSION_SIGNER_ID_LEN 16
#define AKIRA_ADMISSION_SEAL_LEN      32

/** Package signature algorithm names (`sig.<name>` entries in an .akpkg). */
#define AKIRA_SIG_ALG_ED25519  "ed25519"
#define AKIRA_SIG_ALG_MLDSA44  "mldsa44"

/**
 * Admission record, written by the core at install and handed back to every
 * provider at LOAD. The core binds it to the binary (and the stored manifest)
 * by hash; a provider that needs it to resist an attacker with filesystem
 * write access fills `seal` at INSTALL (e.g. an HMAC with a hardware key) and
 * checks it at LOAD.
 */
struct akira_admission_record {
    uint32_t magic;
    uint8_t version;
    uint8_t source;   /**< enum akira_app_source */
    uint8_t attested; /**< 1 if attested at install */
    uint8_t reserved;
    uint8_t binary_sha256[32];
    /** SHA-256 of the stored manifest JSON, all-zero if none was stored. */
    uint8_t manifest_sha256[32];
    uint64_t cap_clamp; /**< ANDed with the granted mask at every load */
    uint8_t signer_id[AKIRA_ADMISSION_SIGNER_ID_LEN];
    uint8_t seal[AKIRA_ADMISSION_SEAL_LEN];
};

/** What a provider is asked to judge. */
struct akira_admission_ctx {
    enum akira_admission_phase phase;
    enum akira_app_source source;
    const char *name;
    const char *version; /**< manifest version, "" if unknown */
    uint64_t requested_caps;
    /** SHA-256 of the WASM/AOT binary. */
    const uint8_t *binary_sha256;
    /** SHA-256 of the manifest JSON that will be stored, or NULL. */
    const uint8_t *manifest_sha256;
    /** True if the binary carries an embedded .akira.manifest section. */
    bool manifest_embedded;
    /** The manifest's "network_policy" (present=false if none), or NULL. */
    const struct akira_net_policy_decl *net_policy;

    /* INSTALL only — NULL / 0 otherwise. */
    /** SHA-256(manifest.json || app binary [|| model.tflite]): what the
     *  package signatures sign. NULL for a bare .wasm install. */
    const uint8_t *package_digest;
    const uint8_t *sig_ed25519; /**< 64 bytes, or NULL */
    size_t sig_ed25519_len;
    const uint8_t *sig_mldsa44; /**< FIPS 204 ML-DSA-44, or NULL */
    size_t sig_mldsa44_len;
    /** Package data the signatures cover, for providers that verify with
     *  pre-hash-free schemes. NULL for a bare .wasm install. */
    const uint8_t *manifest_json;
    size_t manifest_json_len;

    /* LOAD only. */
    /** Record written at install, or NULL (no record: SD-XIP, cloud, legacy
     *  install). The core has already checked its hashes against this load. */
    const struct akira_admission_record *record;
};

/** What providers decide. Initialised by the core before the first provider. */
struct akira_admission_result {
    bool attested;
    uint64_t cap_clamp; /**< starts as all ones */
    uint8_t signer_id[AKIRA_ADMISSION_SIGNER_ID_LEN];
    uint8_t seal[AKIRA_ADMISSION_SEAL_LEN]; /**< INSTALL: stored in the record */
    const char *reason; /**< set on DENY, for logs and the audit trail */
};

/**
 * Provider callback. Returns an enum akira_admission_verdict. May set
 * `res->attested` (only together with ALLOW), narrow `res->cap_clamp`, and at
 * INSTALL fill `res->signer_id` / `res->seal`.
 */
typedef int (*akira_admission_fn)(const struct akira_admission_ctx *ctx,
                                  struct akira_admission_result *res,
                                  void *user_data);

/** One registered provider. Create with AKIRA_APP_ADMISSION_DEFINE(). */
struct akira_app_admission {
    const char *name;
    int16_t prio;
    akira_admission_fn fn;
    void *user_data;
};

/**
 * @brief Register an admission provider.
 *
 * @param id    C identifier, unique across the image.
 * @param prio  Order, lower first. The core baseline is AKIRA_ADMISSION_PRIO_CORE.
 * @param fn    Provider callback.
 * @param user  Opaque pointer passed to @p fn.
 */
#define AKIRA_APP_ADMISSION_DEFINE(id, prio, fn, user)                             \
    const STRUCT_SECTION_ITERABLE(akira_app_admission, id) = {                     \
        #id, (prio), (fn), (user)                                                  \
    }

/**
 * @brief Run every provider over @p ctx. Called by AkiraOS.
 *
 * @return 0 if the app is admitted (see @p res for attestation and clamps),
 *         -EACCES if refused (res->reason says why).
 */
int akira_admission_evaluate(const struct akira_admission_ctx *ctx,
                             struct akira_admission_result *res);

/** Human-readable source name ("http", "ble", ...). */
const char *akira_app_source_name(enum akira_app_source source);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_ADMISSION_H */
