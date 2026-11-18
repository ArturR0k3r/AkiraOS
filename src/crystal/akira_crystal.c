/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_crystal.c
 * @brief Akira's growth state and persistence (see akira_crystal.h).
 */

#include "akira_crystal.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <lib/akira_time.h>
#include <lib/mem_helper.h>
#include <runtime/security.h>
#include <storage/fs_manager.h>

#if defined(CONFIG_AKIRA_SE050)
#include <drivers/secure_element/se050.h>
#include <mbedtls/sha256.h>
#endif

LOG_MODULE_REGISTER(akira_crystal, CONFIG_AKIRA_LOG_LEVEL);

#define DIR_PATH     "/lfs/akira"
#define STATE_PATH   DIR_PATH "/crystal.bin"
#define STATE_TMP    DIR_PATH "/crystal.tmp"
#define CRACKS_PATH  DIR_PATH "/cracks.bin"

#define STATE_MAGIC  0x52434B41u /* 'AKCR' */
#define CRACKS_MAGIC 0x4B524341u /* 'ACRK' */
#define STATE_VERSION 1

#define MAX_PAIRS      40
#define MAX_GLINT_KEYS 48
#define SAVE_DELAY_MS  1500

/* The persisted record — also mirrored verbatim into the SE050. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t days;
    uint32_t generation;
    uint32_t last_day;
    uint32_t uses[AKIRA_DOM_COUNT];
    uint8_t shards[AKIRA_DOM_COUNT];
    uint8_t n_pairs;
    uint8_t n_glint_keys;
    uint16_t pairs[MAX_PAIRS];
    uint32_t glint_keys[MAX_GLINT_KEYS];
    uint16_t glints;
    uint8_t seed[8];
    uint8_t seed_src; /* 0 none, 1 secure element, 2 random */
    uint8_t pad;
    uint32_t ledger_counter;
    uint8_t ledger_tip[16];
    uint32_t crc;
} __packed crystal_disk_t;

typedef struct {
    uint32_t magic;
    uint32_t n;
    akira_crack_t c[AKIRA_MAX_CRACKS];
    uint32_t crc;
} __packed cracks_disk_t;

static K_MUTEX_DEFINE(g_lock);
static atomic_t g_crack_seq;

static struct {
    crystal_disk_t s;
    cracks_disk_t k;
    bool loaded;
    bool persist;      /* /lfs/akira is writable */
    bool dirty_growth;
    bool dirty_cracks;
    int64_t changed_at;
} g AKIRA_BULK_BSS;

/* ---- domains -------------------------------------------------------- */

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static const char *const k_dom_name[AKIRA_DOM_COUNT] = {
    "Radio", "Air", "Net", "Wire", "Vault", "Play",
};

static const uint16_t k_dom_colour[AKIRA_DOM_COUNT] = {
    RGB565(0x8A, 0x4D, 0xFF), /* Radio — violet */
    RGB565(0x3D, 0x7B, 0xFF), /* Air   — blue   */
    RGB565(0x2F, 0xD3, 0xF5), /* Net   — cyan   */
    RGB565(0xFF, 0xB5, 0x47), /* Wire  — amber, the one warm colour */
    RGB565(0xE8, 0xF6, 0xFF), /* Vault — ice    */
    RGB565(0xFF, 0x5F, 0xD2), /* Play  — magenta */
};

akira_domain_t akira_domain_of_cap(uint64_t cap)
{
    if (cap & (AKIRA_CAP_RF_TRANSCEIVE | AKIRA_CAP_MESH | AKIRA_CAP_NFC)) {
        return AKIRA_DOM_RADIO;
    }
    if (cap & (AKIRA_CAP_BLE | AKIRA_CAP_BLE_SCAN | AKIRA_CAP_BLE_SPAM |
               AKIRA_CAP_WIFI_INJECT)) {
        return AKIRA_DOM_AIR;
    }
    if (cap & (AKIRA_CAP_NETWORK | AKIRA_CAP_MATTER | AKIRA_CAP_OTA_TRIGGER)) {
        return AKIRA_DOM_NET;
    }
    if (cap & (AKIRA_CAP_HID | AKIRA_CAP_GPIO_WRITE | AKIRA_CAP_GPIO_READ |
               AKIRA_CAP_UART | AKIRA_CAP_I2C | AKIRA_CAP_PWM | AKIRA_CAP_ADC |
               AKIRA_CAP_INPUT_WRITE)) {
        return AKIRA_DOM_WIRE;
    }
    if (cap & AKIRA_CAP_CRYPTO) {
        return AKIRA_DOM_VAULT;
    }
    return AKIRA_DOM_NONE;
}

const char *akira_domain_name(akira_domain_t d)
{
    return (d >= 0 && d < AKIRA_DOM_COUNT) ? k_dom_name[d] : "Core";
}

uint16_t akira_domain_colour(akira_domain_t d)
{
    return (d >= 0 && d < AKIRA_DOM_COUNT) ? k_dom_colour[d] : 0xFFFFu;
}

/* ---- persistence ------------------------------------------------------ */

static uint32_t state_crc(const crystal_disk_t *s)
{
    return crc32_ieee((const uint8_t *)s, offsetof(crystal_disk_t, crc));
}

static uint32_t cracks_crc(const cracks_disk_t *k)
{
    return crc32_ieee((const uint8_t *)k, offsetof(cracks_disk_t, crc));
}

static bool state_valid(const crystal_disk_t *s)
{
    return s->magic == STATE_MAGIC && s->version == STATE_VERSION && s->crc == state_crc(s);
}

/* -EAGAIN: /lfs isn't mounted yet.  0: usable.  -ENOSPC etc: mounted but
 * Akira can't keep files there (she then lives in RAM this boot). */
static int storage_ready(void)
{
    struct fs_dirent ent;

    if (fs_stat("/lfs", &ent) != 0) {
        return -EAGAIN;
    }
    if (fs_stat(DIR_PATH, &ent) != 0) {
        int ret = fs_mkdir(DIR_PATH);

        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}

static int read_exact(const char *path, void *buf, size_t len)
{
    ssize_t n = fs_manager_read_file(path, buf, len);

    return n == (ssize_t)len ? 0 : -EIO;
}

/* Atomic replace: write a temp file, then rename over the old one. */
static int write_atomic(const char *path, const char *tmp, const void *buf, size_t len)
{
    struct fs_file_t f;
    int ret;

    fs_file_t_init(&f);
    ret = fs_open(&f, tmp, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0) {
        return ret;
    }
    ret = fs_write(&f, buf, len);
    fs_close(&f);
    if (ret != (int)len) {
        return ret < 0 ? ret : -EIO;
    }
    return fs_rename(tmp, path);
}

#if defined(CONFIG_AKIRA_SE050)
static const struct device *secure_element(void)
{
    const struct device *se = se050_get_device();

    return (se && device_is_ready(se)) ? se : NULL;
}

static int se_read_state(crystal_disk_t *out)
{
    const struct device *se = secure_element();
    size_t got = 0;

    if (!se) {
        return -ENODEV;
    }
    int ret = se050_read_binary(se, CONFIG_AKIRA_CRYSTAL_SE050_STATE_OBJID, (uint8_t *)out,
                                sizeof(*out), &got);

    return (ret == 0 && got == sizeof(*out) && state_valid(out)) ? 0 : -ENOENT;
}

static void se_write_state(const crystal_disk_t *s)
{
    const struct device *se = secure_element();

    if (se && se050_write_binary(se, CONFIG_AKIRA_CRYSTAL_SE050_STATE_OBJID,
                                 (const uint8_t *)s, sizeof(*s)) != 0) {
        LOG_WRN("Akira: SE050 mirror write failed");
    }
}

/* The shape seed: SHA-256 of the seal key's public key — the same for the
 * life of the console, whatever happens to its flash. */
static bool se_seed(uint8_t seed[8])
{
    const struct device *se = secure_element();
    uint8_t pub[SE050_P256_PUB_LEN], h[32];

    if (!se || se050_ecc_gen_key(se, CONFIG_AKIRA_CRYSTAL_SE050_KEY_OBJID) != 0 ||
        se050_ecc_get_pub(se, CONFIG_AKIRA_CRYSTAL_SE050_KEY_OBJID, pub) != 0) {
        return false;
    }
    mbedtls_sha256(pub, sizeof(pub), h, 0);
    memcpy(seed, h, 8);
    return true;
}
#endif

static void fresh_state(crystal_disk_t *s)
{
    memset(s, 0, sizeof(*s));
    s->magic = STATE_MAGIC;
    s->version = STATE_VERSION;
}

int akira_crystal_init(void)
{
    if (g.loaded) {
        return 0;
    }
    int st = storage_ready();

    if (st == -EAGAIN) {
        return -EAGAIN;
    }
    if (st != 0) {
        LOG_WRN("Akira: can't keep files in " DIR_PATH " (%d) — growth lives in RAM this boot",
                st);
    }

    static crystal_disk_t AKIRA_BULK_BSS flash_copy;
    crystal_disk_t *best = NULL;

    k_mutex_lock(&g_lock, K_FOREVER);
    if (read_exact(STATE_PATH, &flash_copy, sizeof(flash_copy)) == 0 &&
        state_valid(&flash_copy)) {
        best = &flash_copy;
    }
#if defined(CONFIG_AKIRA_SE050)
    static crystal_disk_t AKIRA_BULK_BSS se_copy;

    if (se_read_state(&se_copy) == 0 && (!best || se_copy.generation > best->generation)) {
        if (!best) {
            LOG_INF("Akira: restored from the secure element");
        }
        best = &se_copy;
    }
#endif
    if (best) {
        g.s = *best;
    } else {
        fresh_state(&g.s);
        g.dirty_growth = true;
    }

    if (g.s.seed_src != 1) {
#if defined(CONFIG_AKIRA_SE050)
        if (se_seed(g.s.seed)) {
            g.s.seed_src = 1;
            g.dirty_growth = true;
        }
#endif
        if (g.s.seed_src == 0) {
            sys_rand_get(g.s.seed, sizeof(g.s.seed));
            g.s.seed_src = 2;
            g.dirty_growth = true;
        }
    }

    if (read_exact(CRACKS_PATH, &g.k, sizeof(g.k)) != 0 || g.k.magic != CRACKS_MAGIC ||
        g.k.crc != cracks_crc(&g.k) || g.k.n > AKIRA_MAX_CRACKS) {
        memset(&g.k, 0, sizeof(g.k));
        g.k.magic = CRACKS_MAGIC;
    }
    g.persist = (st == 0);
    g.loaded = true;
    g.changed_at = k_uptime_get() - SAVE_DELAY_MS;
    k_mutex_unlock(&g_lock);

    LOG_INF("Akira: day %u, %u seals, seed %02x%02x%02x%02x (%s)", g.s.days,
            g.s.ledger_counter, g.s.seed[0], g.s.seed[1], g.s.seed[2], g.s.seed[3],
            g.s.seed_src == 1 ? "secure element" : "random");
    return 0;
}

static void save_locked(void)
{
    if (!g.loaded) {
        return;
    }
    if (g.dirty_growth) {
        g.s.generation++;
        g.s.crc = state_crc(&g.s);
        if (g.persist && write_atomic(STATE_PATH, STATE_TMP, &g.s, sizeof(g.s)) != 0) {
            LOG_WRN("Akira: state save failed");
        }
#if defined(CONFIG_AKIRA_SE050)
        se_write_state(&g.s);
#endif
        g.dirty_growth = false;
    }
    if (g.dirty_cracks) {
        g.k.magic = CRACKS_MAGIC;
        g.k.crc = cracks_crc(&g.k);
        if (g.persist &&
            write_atomic(CRACKS_PATH, DIR_PATH "/cracks.tmp", &g.k, sizeof(g.k)) != 0) {
            LOG_WRN("Akira: cracks save failed");
        }
        g.dirty_cracks = false;
    }
}

void akira_crystal_flush(void)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    save_locked();
    k_mutex_unlock(&g_lock);
}

static void touch(bool growth)
{
    if (growth) {
        g.dirty_growth = true;
    } else {
        g.dirty_cracks = true;
    }
    g.changed_at = k_uptime_get();
}

/* ---- reading ------------------------------------------------------------ */

void akira_crystal_get(akira_crystal_view_t *out)
{
    memset(out, 0, sizeof(*out));
    k_mutex_lock(&g_lock, K_FOREVER);
    out->loaded = g.loaded;
    out->days = g.s.days;
    for (int d = 0; d < AKIRA_DOM_COUNT; d++) {
        out->uses[d] = g.s.uses[d];
        out->shards[d] = g.s.shards[d];
    }
    out->glints = g.s.glints;
    out->seals = g.s.ledger_counter;
    memcpy(out->seed, g.s.seed, sizeof(out->seed));
    for (uint32_t i = 0; i < g.k.n; i++) {
        int d = g.k.c[i].domain;

        if (d >= 0 && d < AKIRA_DOM_COUNT && out->cracks[d] < 255) {
            out->cracks[d]++;
        }
    }
    out->n_cracks = (uint8_t)g.k.n;
    k_mutex_unlock(&g_lock);
}

/* ---- growth ------------------------------------------------------------- */

static size_t name_len(const char *s)
{
    size_t n = 0;

    while (n < AKIRA_APP_NAME_LEN && s[n]) {
        n++;
    }
    return n;
}

static uint16_t key16(const char *app, uint32_t salt)
{
    uint16_t c = crc16_ccitt(0xFFFF, (const uint8_t *)app, name_len(app));

    return crc16_ccitt(c, (const uint8_t *)&salt, sizeof(salt));
}

bool akira_crystal_add_use(akira_domain_t d, const char *app, uint32_t op_id)
{
    bool new_shard = false;

    if (d < 0 || d >= AKIRA_DOM_COUNT || !app) {
        return false;
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    if (g.s.uses[d] < UINT32_MAX) {
        g.s.uses[d]++;
    }

    uint16_t k = key16(app, op_id);
    bool seen = false;

    for (int i = 0; i < g.s.n_pairs; i++) {
        if (g.s.pairs[i] == k) {
            seen = true;
            break;
        }
    }
    if (!seen) {
        if (g.s.n_pairs < MAX_PAIRS) {
            g.s.pairs[g.s.n_pairs++] = k;
        }
        if (g.s.shards[d] < AKIRA_MAX_SHARDS_PER_DOMAIN) {
            g.s.shards[d]++;
            new_shard = true;
        }
    }
    touch(true);
    k_mutex_unlock(&g_lock);
    return new_shard;
}

bool akira_crystal_pair_known(akira_domain_t d, const char *app, uint32_t op_id)
{
    bool known = true;

    if (d < 0 || d >= AKIRA_DOM_COUNT || !app) {
        return true;
    }
    uint16_t k = key16(app, op_id);

    k_mutex_lock(&g_lock, K_FOREVER);
    if (g.s.shards[d] < AKIRA_MAX_SHARDS_PER_DOMAIN) {
        known = false;
        for (int i = 0; i < g.s.n_pairs; i++) {
            if (g.s.pairs[i] == k) {
                known = true;
                break;
            }
        }
    }
    k_mutex_unlock(&g_lock);
    return known;
}

bool akira_crystal_add_glint(const char *app, uint32_t id)
{
    if (!app) {
        return false;
    }
    uint32_t key = ((uint32_t)key16(app, 0x474C4E54u) << 16) | key16(app, id);
    int per_app = 0;
    bool ok = false;

    k_mutex_lock(&g_lock, K_FOREVER);
    for (int i = 0; i < g.s.n_glint_keys; i++) {
        if (g.s.glint_keys[i] == key) {
            goto out; /* already awarded */
        }
        if ((g.s.glint_keys[i] >> 16) == (key >> 16)) {
            per_app++;
        }
    }
    if (per_app >= AKIRA_MAX_GLINTS_PER_APP || g.s.n_glint_keys >= MAX_GLINT_KEYS) {
        goto out;
    }
    g.s.glint_keys[g.s.n_glint_keys++] = key;
    g.s.glints++;
    touch(true);
    ok = true;
out:
    k_mutex_unlock(&g_lock);
    return ok;
}

void akira_crystal_note_denial(const char *app, uint64_t cap)
{
    akira_domain_t d = akira_domain_of_cap(cap);

    if (d == AKIRA_DOM_NONE || !app) {
        return; /* everyday capabilities don't crack Akira */
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    for (uint32_t i = 0; i < g.k.n; i++) {
        akira_crack_t *c = &g.k.c[i];

        if (c->cap == cap && strncmp(c->app, app, sizeof(c->app)) == 0) {
            if (c->count < UINT16_MAX) {
                c->count++; /* a polling app: count, don't re-save every frame */
            }
            k_mutex_unlock(&g_lock);
            return;
        }
    }
    if (g.k.n >= AKIRA_MAX_CRACKS) {
        /* Full: the oldest crack makes room. */
        memmove(&g.k.c[0], &g.k.c[1], sizeof(g.k.c[0]) * (AKIRA_MAX_CRACKS - 1));
        g.k.n = AKIRA_MAX_CRACKS - 1;
    }
    akira_crack_t *c = &g.k.c[g.k.n++];

    memset(c, 0, sizeof(*c));
    strncpy(c->app, app, sizeof(c->app) - 1);
    c->cap = cap;
    c->domain = (int8_t)d;
    c->count = 1;
    c->uptime_s = (uint32_t)(k_uptime_get() / 1000);
    c->epoch = akira_time_is_set() ? akira_time_get_epoch() : 0;
    touch(false);
    atomic_inc(&g_crack_seq);
    k_mutex_unlock(&g_lock);
    LOG_WRN("Akira cracked: %s tried %s", app, akira_capability_mask_to_str(cap));
}

uint32_t akira_crystal_crack_seq(void)
{
    return (uint32_t)atomic_get(&g_crack_seq);
}

int akira_crystal_get_cracks(akira_crack_t *out, int max)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    int n = MIN((int)g.k.n, max);

    memcpy(out, g.k.c, sizeof(akira_crack_t) * n);
    k_mutex_unlock(&g_lock);
    return n;
}

void akira_crystal_mend(int index)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    if (index < 0) {
        g.k.n = 0;
    } else if (index < (int)g.k.n) {
        memmove(&g.k.c[index], &g.k.c[index + 1],
                sizeof(g.k.c[0]) * (g.k.n - index - 1));
        g.k.n--;
    }
    touch(false);
    save_locked();
    k_mutex_unlock(&g_lock);
}

void akira_crystal_set_ledger_tip(uint32_t counter, const uint8_t tip[16])
{
    k_mutex_lock(&g_lock, K_FOREVER);
    g.s.ledger_counter = counter;
    memcpy(g.s.ledger_tip, tip, 16);
    touch(true);
    k_mutex_unlock(&g_lock);
}

void akira_crystal_get_ledger_tip(uint32_t *counter, uint8_t tip[16])
{
    k_mutex_lock(&g_lock, K_FOREVER);
    *counter = g.s.ledger_counter;
    memcpy(tip, g.s.ledger_tip, 16);
    k_mutex_unlock(&g_lock);
}

void akira_crystal_set_seed(const uint8_t seed[8])
{
    k_mutex_lock(&g_lock, K_FOREVER);
    memcpy(g.s.seed, seed, 8);
    g.s.seed_src = 1;
    touch(true);
    k_mutex_unlock(&g_lock);
}

void akira_crystal_tick(void)
{
    if (!g.loaded && akira_crystal_init() != 0) {
        return;
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    if (akira_time_is_set()) {
        int64_t local = akira_time_get_epoch() + akira_time_get_tz_offset_s();
        uint32_t day = (uint32_t)(local / 86400);

        if (day > g.s.last_day) {
            if (g.s.days < UINT16_MAX) {
                g.s.days++;
            }
            g.s.last_day = day;
            touch(true);
        }
    }
    if ((g.dirty_growth || g.dirty_cracks) &&
        k_uptime_get() - g.changed_at >= SAVE_DELAY_MS) {
        save_locked();
    }
    k_mutex_unlock(&g_lock);
}

/* ---- test helpers ------------------------------------------------------- */

#if defined(CONFIG_AKIRA_CRYSTAL_DEBUG)
void akira_crystal_debug_grow(akira_domain_t d, int shards, int uses)
{
    if (d < 0 || d >= AKIRA_DOM_COUNT) {
        return;
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    g.s.shards[d] = (uint8_t)CLAMP(g.s.shards[d] + shards, 0, AKIRA_MAX_SHARDS_PER_DOMAIN);
    g.s.uses[d] += (uint32_t)MAX(uses, 0);
    touch(true);
    k_mutex_unlock(&g_lock);
}

void akira_crystal_debug_days(int days)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    g.s.days = (uint16_t)CLAMP((int)g.s.days + days, 0, UINT16_MAX);
    touch(true);
    k_mutex_unlock(&g_lock);
}

void akira_crystal_debug_glints(int n)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    g.s.glints = (uint16_t)CLAMP((int)g.s.glints + n, 0, UINT16_MAX);
    touch(true);
    k_mutex_unlock(&g_lock);
}

void akira_crystal_debug_reset(void)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    uint8_t seed[8];
    uint8_t src = g.s.seed_src;

    memcpy(seed, g.s.seed, sizeof(seed));
    fresh_state(&g.s);
    memcpy(g.s.seed, seed, sizeof(seed));
    g.s.seed_src = src;
    memset(&g.k, 0, sizeof(g.k));
    touch(true);
    touch(false);
    k_mutex_unlock(&g_lock);
}
#endif
