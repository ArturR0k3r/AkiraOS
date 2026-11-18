/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_crystal.h
 * @brief Akira — the console's crystal, grown from how it is used.
 *
 * Anatomy (it mirrors the AkiraOS logo, which is a fully grown Akira):
 *   - core    the bright centre; grows with the number of days used
 *   - shards  the spikes; one per (app, power) ever sealed, in that power's
 *             domain colour, longer the more it is used
 *   - glints  floating fragments; achievements awarded by games
 *   - cracks  an app tried a power it never declared and was refused;
 *             shown until the owner mends them
 *
 * Growth is logarithmic and never decays: no grind, no guilt.
 *
 * Persistence: /lfs/akira/crystal.bin, mirrored into an SE050 binary object
 * when one is fitted so a flash wipe doesn't erase it (the newer generation
 * wins on load).  Cracks live only in flash — a reset clears them.
 *
 * Thread safety: every function may be called from any thread.  Calls from
 * WASM threads (akira_crystal_note_denial) only touch RAM; saving is
 * deferred to akira_crystal_tick(), which the shell runs once a second.
 */

#ifndef AKIRA_CRYSTAL_H
#define AKIRA_CRYSTAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Power domains — each grows shards in its own colour. */
typedef enum {
    AKIRA_DOM_RADIO = 0, /**< RF transmit, LoRa mesh, NFC */
    AKIRA_DOM_AIR,       /**< BLE, BLE scan/spam, Wi-Fi injection */
    AKIRA_DOM_NET,       /**< network, Matter, OTA, app installs */
    AKIRA_DOM_WIRE,      /**< HID, GPIO, UART, I2C, PWM, ADC, USB sharing */
    AKIRA_DOM_VAULT,     /**< keys held in the secure element */
    AKIRA_DOM_PLAY,      /**< games: sealed records */
    AKIRA_DOM_COUNT,
    AKIRA_DOM_NONE = -1, /**< everyday capability: feeds no shard */
} akira_domain_t;

#define AKIRA_MAX_SHARDS_PER_DOMAIN 6
#define AKIRA_MAX_CRACKS 8
#define AKIRA_MAX_GLINTS_PER_APP 5
#define AKIRA_APP_NAME_LEN 24

/** One unresolved crack: an app tried a power outside its manifest. */
typedef struct {
    char app[AKIRA_APP_NAME_LEN];
    uint64_t cap;    /**< the capability bit it was refused */
    int64_t epoch;   /**< wall clock of the first attempt (0 if unset) */
    uint32_t uptime_s;
    uint16_t count;  /**< attempts since the crack appeared */
    int8_t domain;
} akira_crack_t;

/** Read-only snapshot for renderers and screens. */
typedef struct {
    uint16_t days;
    uint32_t uses[AKIRA_DOM_COUNT];
    uint8_t shards[AKIRA_DOM_COUNT];
    uint16_t glints;
    uint8_t cracks[AKIRA_DOM_COUNT]; /**< unresolved cracks per domain */
    uint8_t n_cracks;
    uint32_t seals;                  /**< ledger entries so far */
    uint8_t seed[8];                 /**< per-console shape seed */
    bool loaded;
} akira_crystal_view_t;

/** Load state (idempotent; retried by the tick until storage is up). */
int akira_crystal_init(void);

/** Fill @p out with the current state. */
void akira_crystal_get(akira_crystal_view_t *out);

/** Domain of a capability bit, or AKIRA_DOM_NONE for everyday ones. */
akira_domain_t akira_domain_of_cap(uint64_t cap);

/** "Radio", "Air", ... */
const char *akira_domain_name(akira_domain_t d);

/** RGB565 colour of a domain's shards (colour panels). */
uint16_t akira_domain_colour(akira_domain_t d);

/**
 * A seal (or an always-grant's first use in a run) for (@p app, @p op_id) in
 * @p d.  A pair never seen before grows a new shard.
 * @return true if a new shard appeared.
 */
bool akira_crystal_add_use(akira_domain_t d, const char *app, uint32_t op_id);

/** True if sealing (@p app, @p op_id) would not grow a new shard. */
bool akira_crystal_pair_known(akira_domain_t d, const char *app, uint32_t op_id);

/**
 * Award a glint for achievement @p id of @p app (deduplicated, at most
 * AKIRA_MAX_GLINTS_PER_APP per app).  @return true if it was new.
 */
bool akira_crystal_add_glint(const char *app, uint32_t id);

/** The runtime refused @p app capability @p cap (call from any thread). */
void akira_crystal_note_denial(const char *app, uint64_t cap);

/** Bumped every time a new crack appears (for "Akira blocked ..." notices). */
uint32_t akira_crystal_crack_seq(void);

/** Copy up to @p max unresolved cracks into @p out; returns the count. */
int akira_crystal_get_cracks(akira_crack_t *out, int max);

/** Mend crack @p index (from akira_crystal_get_cracks order); -1 = all. */
void akira_crystal_mend(int index);

/** Record the ledger head after a seal (counter + chain hash). */
void akira_crystal_set_ledger_tip(uint32_t counter, const uint8_t tip[16]);
void akira_crystal_get_ledger_tip(uint32_t *counter, uint8_t tip[16]);

/** Seed the shape from the SE050 seal key's public key (stable for life). */
void akira_crystal_set_seed(const uint8_t seed[8]);

/** Once a second (shell thread): day counting, deferred saves. */
void akira_crystal_tick(void);

/** Save now if anything changed (shell thread). */
void akira_crystal_flush(void);

#if defined(CONFIG_AKIRA_CRYSTAL_DEBUG)
/** Test helpers: fake growth without a seal. */
void akira_crystal_debug_grow(akira_domain_t d, int shards, int uses);
void akira_crystal_debug_days(int days);
void akira_crystal_debug_glints(int n);
void akira_crystal_debug_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CRYSTAL_H */
