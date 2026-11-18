/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_seal.h
 * @brief The Seal — consent for restricted powers, owned by the OS.
 *
 * A restricted power runs only under a live grant:
 *   - THIS TIME  until the app stops (or, for a system action, just once);
 *   - ALWAYS     for this app build and this power, until revoked.
 *
 * Granting takes a Seal: the owner holds A+B while a ring charges (one lap
 * = this time, a second lap = always).  The decision is written to an
 * append-only, hash-chained ledger and signed by the SE050 when one is
 * fitted, so every grant can be audited and verified later.
 *
 * Enforcement sits in the native APIs (akira_seal_gate() right after the
 * capability check), so an app can't route around the prompt.  Apps that
 * want to explain themselves call seal_request() first with a reason.
 *
 * The UI is supplied by the OS shell (akira_seal_set_ui()); without one
 * every request is refused.
 */

#ifndef AKIRA_SEAL_H
#define AKIRA_SEAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>

#include "akira_crystal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sealable operations.  Values are ABI: WASM apps pass them to seal_request(). */
typedef enum {
    AKIRA_SEAL_NONE = 0,
    AKIRA_SEAL_RF_TX = 1,       /**< transmit on a sub-GHz radio */
    AKIRA_SEAL_WIFI_INJECT = 2, /**< inject raw Wi-Fi frames (deauth, PMKID) */
    AKIRA_SEAL_BLE_SPAM = 3,    /**< broadcast spoofed BLE advertisements */
    AKIRA_SEAL_HID_INJECT = 4,  /**< type / click / move as a USB-BLE HID */
    AKIRA_SEAL_VAULT_SIGN = 5,  /**< sign with a key held in the SE050 */
    AKIRA_SEAL_APP_INSTALL = 6, /**< install an app (system) */
    AKIRA_SEAL_USB_SHARE = 7,   /**< share the SD card with a USB host (system) */
    AKIRA_SEAL_RECORD = 8,      /**< seal a game record (score, time) */
    AKIRA_SEAL_OP_COUNT
} akira_seal_op_t;

typedef enum {
    AKIRA_SCOPE_REFUSED = 0,
    AKIRA_SCOPE_ONCE = 1,
    AKIRA_SCOPE_ALWAYS = 2,
    AKIRA_SCOPE_TIMEOUT = 3,
} akira_seal_scope_t;

/** App name used for seals the OS itself asks for. */
#define AKIRA_SEAL_SYSTEM_APP "AkiraOS"

#define AKIRA_SEAL_DETAIL_LEN 48

/** One ledger entry: the signed record and its signature. */
typedef struct {
    uint32_t magic;   /**< 'AKSL' */
    uint16_t version; /**< 1 */
    uint8_t op;
    uint8_t scope;
    uint32_t counter; /**< monotonic, from 1 */
    int64_t epoch;    /**< wall clock (0 if the clock was never set) */
    uint32_t uptime_s;
    uint32_t app_id;  /**< build identity the grant is bound to */
    char app[AKIRA_APP_NAME_LEN];
    char detail[AKIRA_SEAL_DETAIL_LEN];
    uint8_t prev[16]; /**< first 16 bytes of SHA-256 of the previous record */
} __packed akira_seal_record_t;

typedef struct {
    akira_seal_record_t rec;
    uint8_t sig[64];  /**< ECDSA P-256 r||s over SHA-256(rec), SE050 key */
    uint8_t signed_by_se; /**< 0 = no secure element, sig is zero */
    uint8_t pad[3];
} __packed akira_seal_entry_t;

/** A request travelling from the asker to the UI and back. */
typedef struct akira_seal_req {
    akira_seal_op_t op;
    char app[AKIRA_APP_NAME_LEN];
    char detail[AKIRA_SEAL_DETAIL_LEN];
    bool allow_always;
    bool (*should_cancel)(void);      /**< optional, polled by the UI */
    /* filled by akira_seal_finish() */
    akira_seal_scope_t scope;
    bool new_shard;
    akira_seal_entry_t entry;
} akira_seal_req_t;

/* ---- static descriptions -------------------------------------------- */

akira_domain_t akira_seal_op_domain(akira_seal_op_t op);
/** "Transmit on radio" */
const char *akira_seal_op_title(akira_seal_op_t op);
/** True for OS-originated actions (they never offer ALWAYS). */
bool akira_seal_op_is_system(akira_seal_op_t op);

/* ---- asking ------------------------------------------------------------ */

/**
 * Block the calling (app) thread until @p app holds a grant for @p op —
 * immediately if it already does, else after the owner answers a Seal.
 * @return 0 granted, -EACCES refused, -ETIMEDOUT unanswered, -EBUSY no UI.
 * Never call from the shell thread (use akira_seal_ask()).
 */
int akira_seal_require(const char *app, akira_seal_op_t op, const char *detail);

/**
 * Non-blocking variant for the shell's own flows.  @p on_result runs on the
 * shell thread with the answer.  System ops grant THIS TIME only and are
 * consumed by the action.
 */
void akira_seal_ask(akira_seal_op_t op, const char *detail, bool (*should_cancel)(void),
                    void (*on_result)(bool granted, void *ctx), void *ctx);

/**
 * Seal a record for @p app (blocking, app thread).  Unlike powers, every
 * record is its own ledger entry; an ALWAYS grant just skips the prompt.
 * @return 0 sealed, -EACCES refused, ...
 */
int akira_seal_record(const char *app, const char *what, int64_t value);

/**
 * True if @p app may use @p op right now without asking.  The first use of
 * an ALWAYS grant in a run checks the app build still matches and feeds
 * Akira once.  Any thread except while holding the app registry mutex.
 */
bool akira_seal_granted(const char *app, akira_seal_op_t op);

/* ---- UI side ----------------------------------------------------------- */

/**
 * The shell registers a presenter.  It is called from the asking thread and
 * must hand @p req to the UI asynchronously; the UI later calls
 * akira_seal_finish() exactly once, on the shell thread.
 */
void akira_seal_set_ui(int (*present)(akira_seal_req_t *req));

/** The shell's "take the seal screen down now" hook (any thread). */
void akira_seal_set_ui_cancel(void (*cancel)(void));

/**
 * @p app is being stopped or has died: if it is waiting on a Seal, refuse
 * it now (the app thread wakes) and ask the UI to close.  Any thread.
 */
void akira_seal_abandon_app(const char *app);

/**
 * The owner answered.  Records the grant, signs and appends the ledger entry
 * (fills req->entry, for the stamp's sigil) and grows Akira.  Shell thread.
 */
void akira_seal_finish(akira_seal_req_t *req, akira_seal_scope_t scope);

/**
 * The seal screen has closed (and the display is back with the app): wake
 * the asker, or run the shell's callback.  Exactly once per request.
 */
void akira_seal_release(akira_seal_req_t *req);

/* ---- lifecycle / management -------------------------------------------- */

/** An app stopped: its THIS TIME grants end. */
void akira_seal_app_stopped(const char *app);

/** ALWAYS grants, for the "Kept seals" list. */
typedef struct {
    char app[AKIRA_APP_NAME_LEN];
    uint16_t ops; /**< BIT(op) */
} akira_seal_grant_t;

int akira_seal_list_grants(akira_seal_grant_t *out, int max);
/** Revoke @p app's ALWAYS grant for @p op (AKIRA_SEAL_NONE = all of them). */
void akira_seal_revoke(const char *app, akira_seal_op_t op);

/** Newest-first ledger read: entry @p index_from_newest (0 = newest). */
int akira_seal_ledger_read(int index_from_newest, akira_seal_entry_t *out);
/** Number of entries in the current ledger file. */
int akira_seal_ledger_count(void);

/** 16-byte seed for an entry's sigil (derived from its signature/digest). */
void akira_seal_sigil_seed(const akira_seal_entry_t *e, uint8_t seed[16]);

/** Seal key public key (65 bytes, uncompressed); -ENODEV without an SE050. */
int akira_seal_pubkey(uint8_t pub[65]);

/** True if seals are signed by a secure element on this console. */
bool akira_seal_has_secure_element(void);

/* ---- enforcement in native APIs ------------------------------------------ */

#if defined(CONFIG_AKIRA_CRYSTAL) && defined(CONFIG_AKIRA_WASM_RUNTIME)
#include <wasm_export.h>

/**
 * Gate a restricted native: 0 if the calling app holds (or is now given) a
 * grant for @p op, else a negative errno.  Blocks the app while it asks.
 */
int akira_seal_gate(wasm_exec_env_t exec_env, akira_seal_op_t op, const char *detail);

/* WASM natives (akira_seal_api.c), registered in akira_export_api.c. */
int akira_native_seal_request(wasm_exec_env_t exec_env, int32_t op, const char *reason);
int akira_native_akira_glint(wasm_exec_env_t exec_env, int32_t id);
int akira_native_seal_record(wasm_exec_env_t exec_env, const char *what, int64_t value);

/** Place right after the capability check of a restricted native. */
#define AKIRA_SEAL_GATE_OR_RETURN(env, op, detail, ret)                                \
    do {                                                                               \
        if (akira_seal_gate((env), (op), (detail)) != 0) {                             \
            return (ret);                                                              \
        }                                                                              \
    } while (0)
#else
#define AKIRA_SEAL_GATE_OR_RETURN(env, op, detail, ret) do { } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SEAL_H */
