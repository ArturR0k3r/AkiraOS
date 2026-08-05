/**
 * @file security.h
 * @stability stable
 * @since 1.3
 */
#ifndef AKIRA_RUNTIME_SECURITY_H
#define AKIRA_RUNTIME_SECURITY_H

#include <stdbool.h>
#include <stdint.h>

/* WAMR headers are optional; provide lightweight typedefs when WAMR is not enabled */
#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#else
/* Provide opaque types so code can compile when WAMR is disabled (stubs) */
typedef void *wasm_exec_env_t;
typedef void *wasm_module_inst_t;
typedef void *wasm_module_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif


#define AKIRA_CAP_DISPLAY_WRITE (1ULL << 0)
#define AKIRA_CAP_INPUT_READ    (1ULL << 1)
#define AKIRA_CAP_INPUT_WRITE   (1ULL << 2)
#define AKIRA_CAP_SENSOR_READ   (1ULL << 3)
#define AKIRA_CAP_RF_TRANSCEIVE (1ULL << 4)
#define AKIRA_CAP_BLE           (1ULL << 5)  /* BLE app service: init, advertise, GATT */
#define AKIRA_CAP_STORAGE_READ  (1ULL << 6)
#define AKIRA_CAP_STORAGE_WRITE (1ULL << 7)
#define AKIRA_CAP_NETWORK       (1ULL << 8)
#define AKIRA_CAP_GPIO_READ     (1ULL << 9)
#define AKIRA_CAP_GPIO_WRITE    (1ULL << 10)
#define AKIRA_CAP_TIMER         (1ULL << 11)
#define AKIRA_CAP_UART          (1ULL << 12)
#define AKIRA_CAP_I2C           (1ULL << 13)
#define AKIRA_CAP_PWM           (1ULL << 14)
/* Elevated privilege — must not be granted to untrusted apps by default */
#define AKIRA_CAP_HID           (1ULL << 15)
#define AKIRA_CAP_APP_CONTROL   (1ULL << 16)
#define AKIRA_CAP_IPC           (1ULL << 17)
/* Lightweight handoff: start another app and exit self — no stop/pause of
 * arbitrary apps.  Games and utilities can use this without full supervisor
 * power.  Manifest string: "app.switch" */
#define AKIRA_CAP_APP_SWITCH    (1ULL << 18)
/* Quota-enforced heap allocation from WASM-accessible memory.
 * Required to call mem_alloc / mem_free native APIs.
 * Manifest string: "memory" */
#define AKIRA_CAP_MEMORY        (1ULL << 19)
/* Read-only app identity: get own name, list apps, query status.
 * Does NOT grant start/stop authority — use app.control for that.
 * Manifest string: "app.info" */
#define AKIRA_CAP_APP_INFO      (1ULL << 20)
/* Read-only power & battery queries (mode, battery level/status).
 * Manifest string: "power.read" */
#define AKIRA_CAP_POWER_READ    (1ULL << 21)
/* Full power control: set sleep mode, configure wake sources.
 * Elevated privilege — do not grant to untrusted apps.
 * Manifest string: "power.control" */
#define AKIRA_CAP_POWER_CTRL    (1ULL << 22)
/* Persistent key-value settings read/write (NVS-backed).
 * Manifest string: "settings.*" */
#define AKIRA_CAP_SETTINGS      (1ULL << 23)
/* ADC channel read (raw and millivolt).
 * Manifest string: "adc" */
#define AKIRA_CAP_ADC           (1ULL << 24)
/* Feed/pet the system watchdog from a WASM app.
 * Manifest string: "wdt" */
#define AKIRA_CAP_WDT           (1ULL << 25)
/* Filesystem read access (app sandbox only).
 * Manifest string: "fs.read" */
#define AKIRA_CAP_FS_READ       (1ULL << 26)
/* Filesystem write/delete/mkdir access (app sandbox only).
 * Manifest string: "fs.write" */
#define AKIRA_CAP_FS_WRITE      (1ULL << 27)
/* Cryptographic operations (hash, encrypt, random).
 * Manifest string: "crypto" */
#define AKIRA_CAP_CRYPTO        (1ULL << 28)
/* Real-time clock read access.
 * Manifest string: "rtc.read" */
#define AKIRA_CAP_RTC_READ      (1ULL << 29)
/* Real-time clock write / alarm set.
 * Manifest string: "rtc.write" */
#define AKIRA_CAP_RTC_WRITE     (1ULL << 30)
/* Trigger OTA update (elevated privilege).
 * Manifest string: "ota.trigger" */
#define AKIRA_CAP_OTA_TRIGGER   (1ULL << 31)
/* On-device ML inference via TFLite Micro (AkiraClaw).
 * Manifest string: "ai.infer" */
#define AKIRA_CAP_AIINFER       (1ULL << 32)
/* Matter/Thread co-processor IPC bridge.
 * Manifest string: "matter" */
#define AKIRA_CAP_MATTER        (1ULL << 33)
/* 802.11 raw management frame injection (deauth, disassoc).
 * Elevated privilege — requires explicit user consent.
 * Manifest string: "wifi.inject" */
#define AKIRA_CAP_WIFI_INJECT   (1ULL << 34)

/* MQTT publish/subscribe + Home Assistant discovery.
 * Manifest string: "mqtt" */
#define AKIRA_CAP_MQTT          (1ULL << 35)

/* BLE observer role: scan for nearby advertisers (no GATT connect).
 * Manifest string: "ble.scan" */
#define AKIRA_CAP_BLE_SCAN      (1ULL << 36)
/* BLE rotating raw-advertiser (pairing-popup spam/spoof presets).
 * Elevated privilege — broadcasts affect nearby devices, do not grant
 * to untrusted apps by default. Manifest string: "ble.spam" */
#define AKIRA_CAP_BLE_SPAM      (1ULL << 37)

/* AkiraMesh: join the mesh, send/broadcast frames, enumerate nodes.
 * Elevated privilege — the app drives a shared radio and can reach every
 * device in range. Manifest string: "mesh" */
#define AKIRA_CAP_MESH          (1ULL << 38)

/* AkiraSync: open a distributed-ordering session and submit/consume events.
 * Rides on AKIRA_CAP_MESH transport but is granted separately so an app can
 * consume ordered state without being able to inject raw mesh frames.
 * Manifest string: "sync" */
#define AKIRA_CAP_SYNC          (1ULL << 39)

/* Highest capability bit currently defined (AKIRA_CAP_SYNC = bit 39).
 * Keep in sync when adding new AKIRA_CAP_* bits above. */
#define AKIRA_CAP_MAX_BIT       39

/* Union of every capability bit the runtime actually understands. A manifest
 * wildcard ("*") is bounded to this — it can never grant undefined future bits
 * (which UINT64_MAX would have).
 *
 * Derived from AKIRA_CAP_MAX_BIT rather than from a named capability: the
 * previous ((AKIRA_CAP_MQTT << 1) - 1) form stopped at bit 35 and silently
 * dropped every bit added after it. */
#define AKIRA_CAP_ALL_KNOWN     ((AKIRA_CAP_MAX_BIT >= 63) ? UINT64_MAX \
                                 : ((1ULL << (AKIRA_CAP_MAX_BIT + 1)) - 1ULL))

/* Capabilities that let an app affect the world outside the sandbox, persist
 * state, or attack the RF/network environment. Granting any of these to an
 * unattested (unsigned) app is security-sensitive and is audit-logged. */
#define AKIRA_CAP_PRIVILEGED  ( \
        AKIRA_CAP_RF_TRANSCEIVE | AKIRA_CAP_BLE | AKIRA_CAP_NETWORK | \
        AKIRA_CAP_WIFI_INJECT | AKIRA_CAP_OTA_TRIGGER | AKIRA_CAP_HID | \
        AKIRA_CAP_STORAGE_WRITE | AKIRA_CAP_FS_WRITE | AKIRA_CAP_SETTINGS | \
        AKIRA_CAP_CRYPTO | AKIRA_CAP_POWER_CTRL | AKIRA_CAP_MATTER | \
        AKIRA_CAP_MQTT | AKIRA_CAP_WDT | AKIRA_CAP_MESH )

/**
 * @brief Sanitize a capability mask that came from an app-supplied manifest.
 *
 * The manifest is NOT cryptographically bound to a trusted signer on a stock
 * build, so an app can request any capabilities it likes (including the "*"
 * wildcard). This clamps the request:
 *   - always masks to AKIRA_CAP_ALL_KNOWN (drops undefined/high bits);
 *   - when @p attested is false, additionally ANDs with the operator-configured
 *     allow-mask for unsigned apps (CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK) so a
 *     production board can refuse to hand privileged capabilities to code it
 *     cannot attest.
 * Privileged grants to unattested apps are audit-logged by the caller.
 *
 * @param requested  Capability bits parsed from the manifest.
 * @param attested   True if the app passed a real signature/allowlist gate.
 * @return The effective capability mask to grant.
 */
uint64_t akira_capability_sanitize_app_mask(uint64_t requested, bool attested);

/*
 * Capability check macro using security subsystem.
 * Delegates to akira_security_check_exec() for centralized permission validation.
 * For WASM API use with string-based capability names.
 *
 * @param exec_env     The WASM execution environment
 * @param capability   The capability name to check (e.g., AKIRA_CAP_DISPLAY_WRITE)
 * @return             true if capability is granted, false otherwise
 *
 * Note: Involves function call overhead. For performance-critical paths,
 * consider caching capability check results when possible.
 */
#define AKIRA_CHECK_CAP_INLINE(exec_env, capability) \
    akira_security_check_exec(exec_env, capability)

/*
 * Inline capability check with early return for functions returning int.
 * Usage: AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_<CAPABILITY>, -EPERM);
 */
#define AKIRA_CHECK_CAP_OR_RETURN(exec_env, capability, retval) \
    do { \
        if (!akira_security_check_exec(exec_env, capability)) { \
            return (retval); \
        } \
    } while (0)

/*
 * Inline capability check with early return for void functions.
 */
#define AKIRA_CHECK_CAP_OR_RETURN_VOID(exec_env, capability) \
    do { \
        if (!akira_security_check_exec(exec_env, capability)) { \
            return; \
        } \
    } while (0)

/* Central capability guard used by native APIs and runtime */
bool akira_security_check_exec(wasm_exec_env_t exec_env, uint64_t capability);
bool akira_security_check_native(uint64_t capability);

/* Convenience wrapper used by native API implementations (non-wasm callers) */
bool akira_security_check(uint64_t capability);

/* Get the current app's capability mask from exec_env - for use with inline macros */
uint64_t akira_security_get_cap_mask(wasm_exec_env_t exec_env);

/* Capability string to mask helper (public so runtime can parse manifests).
 * This maps capability strings like "display.write" -> AKIRA_CAP_DISPLAY_WRITE
 */
uint64_t akira_capability_str_to_mask(const char *cap);
/* Mask to string helper for logging (returns first matching capability string) */
char* akira_capability_mask_to_str(uint64_t cap);

/* Runtime helpers used by security implementation */
#include <stddef.h>
#include <stdint.h>
uint64_t akira_runtime_get_cap_mask_for_module_inst(wasm_module_inst_t inst);
int akira_runtime_get_name_for_module_inst(wasm_module_inst_t inst, char *buf, size_t buflen);


#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RUNTIME_SECURITY_H */