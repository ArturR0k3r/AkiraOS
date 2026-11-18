/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_seal.c
 * @brief Grants, the request path and the signed ledger (see akira_seal.h).
 *
 * Locks: g_lock guards grants and the in-flight request; g_ledger_lock
 * serializes ledger appends (signing can take ~100 ms on the SE050).  The
 * app manager calls akira_seal_app_stopped() with its registry mutex held,
 * so nothing here ever calls into the app manager while holding g_lock.
 */

#include "akira_seal.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <mbedtls/sha256.h>

#include <lib/akira_time.h>
#include <lib/mem_helper.h>
#include <runtime/app_manager/app_manager.h>
#include <storage/fs_manager.h>

#if defined(CONFIG_AKIRA_SE050)
#include <drivers/secure_element/se050.h>
#endif

LOG_MODULE_REGISTER(akira_seal, CONFIG_AKIRA_LOG_LEVEL);

#define DIR_PATH        "/lfs/akira"
#define LEDGER_PATH     DIR_PATH "/ledger.bin"
#define LEDGER_OLD_PATH DIR_PATH "/ledger.old"
#define GRANTS_PATH     DIR_PATH "/grants.bin"
#define GRANTS_TMP      DIR_PATH "/grants.tmp"

#define SEAL_MAGIC    0x4C534B41u /* 'AKSL' */
#define GRANTS_MAGIC  0x52474B41u /* 'AKGR' */
#define LEDGER_ROTATE 256         /* entries per ledger file */
#define MAX_ALWAYS    16
#define MAX_RUNS      6

/* ---- static op table ----------------------------------------------------- */

static const struct {
    akira_domain_t domain;
    const char *title;
    bool system;
} k_ops[AKIRA_SEAL_OP_COUNT] = {
    [AKIRA_SEAL_NONE] = { AKIRA_DOM_NONE, "Nothing", true },
    [AKIRA_SEAL_RF_TX] = { AKIRA_DOM_RADIO, "Transmit on radio", false },
    [AKIRA_SEAL_WIFI_INJECT] = { AKIRA_DOM_AIR, "Inject Wi-Fi frames", false },
    [AKIRA_SEAL_BLE_SPAM] = { AKIRA_DOM_AIR, "Broadcast BLE spam", false },
    [AKIRA_SEAL_HID_INJECT] = { AKIRA_DOM_WIRE, "Act as keyboard & mouse", false },
    [AKIRA_SEAL_VAULT_SIGN] = { AKIRA_DOM_VAULT, "Sign with a vault key", false },
    [AKIRA_SEAL_APP_INSTALL] = { AKIRA_DOM_NET, "Install an app", true },
    [AKIRA_SEAL_USB_SHARE] = { AKIRA_DOM_WIRE, "Share SD card over USB", true },
    [AKIRA_SEAL_RECORD] = { AKIRA_DOM_PLAY, "Seal a record", false },
};

static bool op_valid(akira_seal_op_t op)
{
    return op > AKIRA_SEAL_NONE && op < AKIRA_SEAL_OP_COUNT;
}

akira_domain_t akira_seal_op_domain(akira_seal_op_t op)
{
    return op_valid(op) ? k_ops[op].domain : AKIRA_DOM_NONE;
}

const char *akira_seal_op_title(akira_seal_op_t op)
{
    return op_valid(op) ? k_ops[op].title : "Unknown power";
}

bool akira_seal_op_is_system(akira_seal_op_t op)
{
    return !op_valid(op) || k_ops[op].system;
}

/* ---- state ---------------------------------------------------------------- */

typedef struct {
    char app[AKIRA_APP_NAME_LEN];
    uint32_t id;  /* app build identity the grant is bound to */
    uint16_t ops; /* BIT(op) */
    uint16_t pad;
} always_t;

typedef struct {
    uint32_t magic;
    uint32_t n;
    always_t a[MAX_ALWAYS];
} grants_disk_t;

/* A running app's THIS TIME grants and the ALWAYS grants it has used. */
typedef struct {
    char app[AKIRA_APP_NAME_LEN];
    uint16_t once;
    uint16_t live_always;
} run_t;

static K_MUTEX_DEFINE(g_lock);
static K_MUTEX_DEFINE(g_ledger_lock);
static K_SEM_DEFINE(g_slot, 1, 1); /* one request in flight */
static K_SEM_DEFINE(g_done, 0, 1); /* the in-flight app request was answered */

static struct {
    akira_seal_req_t req;
    bool from_shell;
    bool abandoned;   /* the asker left (stopped, crashed, timed out): the
                       * slot is freed by release() once the UI has closed */
    bool waiting;     /* an app thread is blocked on g_done */
    void (*cancel_ui)(void);
    void (*on_result)(bool granted, void *ctx);
    void *ctx;
    int (*present)(akira_seal_req_t *req);
    grants_disk_t grants;
    bool grants_loaded;
    run_t runs[MAX_RUNS];
    int8_t have_se; /* -1 unknown */
} g AKIRA_BULK_BSS;

static bool g_have_se_known;

/* ---- grants ---------------------------------------------------------------- */

static void grants_load_locked(void)
{
    if (g.grants_loaded) {
        return;
    }
    memset(&g.grants, 0, sizeof(g.grants));
    if (fs_manager_read_file(GRANTS_PATH, &g.grants, sizeof(g.grants)) !=
            (ssize_t)sizeof(g.grants) ||
        g.grants.magic != GRANTS_MAGIC || g.grants.n > MAX_ALWAYS) {
        memset(&g.grants, 0, sizeof(g.grants));
    }
    g.grants.magic = GRANTS_MAGIC;
    g.grants_loaded = true;
}

static void grants_save_locked(void)
{
    struct fs_file_t f;

    fs_file_t_init(&f);
    if (fs_open(&f, GRANTS_TMP, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC) < 0) {
        LOG_WRN("Seal: can't save grants");
        return;
    }
    int n = fs_write(&f, &g.grants, sizeof(g.grants));

    fs_close(&f);
    if (n == (int)sizeof(g.grants)) {
        fs_rename(GRANTS_TMP, GRANTS_PATH);
    }
}

static always_t *find_always_locked(const char *app)
{
    grants_load_locked();
    for (uint32_t i = 0; i < g.grants.n; i++) {
        if (strncmp(g.grants.a[i].app, app, AKIRA_APP_NAME_LEN) == 0) {
            return &g.grants.a[i];
        }
    }
    return NULL;
}

static run_t *find_run_locked(const char *app, bool create)
{
    run_t *free_slot = NULL;

    for (int i = 0; i < MAX_RUNS; i++) {
        if (g.runs[i].app[0] == '\0') {
            if (!free_slot) {
                free_slot = &g.runs[i];
            }
        } else if (strncmp(g.runs[i].app, app, AKIRA_APP_NAME_LEN) == 0) {
            return &g.runs[i];
        }
    }
    if (create) {
        if (!free_slot) {
            free_slot = &g.runs[0]; /* more live apps than slots: recycle */
        }
        memset(free_slot, 0, sizeof(*free_slot));
        strncpy(free_slot->app, app, AKIRA_APP_NAME_LEN - 1);
        return free_slot;
    }
    return NULL;
}

bool akira_seal_granted(const char *app, akira_seal_op_t op)
{
    if (!app || !op_valid(op)) {
        return false;
    }
    uint32_t bound = 0;
    bool candidate = false;

    k_mutex_lock(&g_lock, K_FOREVER);
    run_t *r = find_run_locked(app, false);

    if (r && ((r->once | r->live_always) & BIT(op))) {
        k_mutex_unlock(&g_lock);
        return true;
    }
    always_t *a = find_always_locked(app);

    if (a && (a->ops & BIT(op))) {
        bound = a->id;
        candidate = true;
    }
    k_mutex_unlock(&g_lock);

    if (!candidate) {
        return false;
    }
    /* Outside g_lock: the app manager takes its registry mutex. */
    if (app_manager_identity(app) != bound) {
        LOG_INF("Seal: %s changed since it was sealed — asking again", app);
        return false;
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    r = find_run_locked(app, true);
    bool first_use = !(r->live_always & BIT(op));

    r->live_always |= BIT(op);
    k_mutex_unlock(&g_lock);

    if (first_use) {
        /* An ALWAYS grant still feeds Akira once per run. */
        akira_crystal_add_use(akira_seal_op_domain(op), app, op);
    }
    return true;
}

void akira_seal_app_stopped(const char *app)
{
    if (!app) {
        return;
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    run_t *r = find_run_locked(app, false);

    if (r) {
        memset(r, 0, sizeof(*r));
    }
    k_mutex_unlock(&g_lock);
}

int akira_seal_list_grants(akira_seal_grant_t *out, int max)
{
    int n = 0;

    k_mutex_lock(&g_lock, K_FOREVER);
    grants_load_locked();
    for (uint32_t i = 0; i < g.grants.n && n < max; i++) {
        strncpy(out[n].app, g.grants.a[i].app, AKIRA_APP_NAME_LEN);
        out[n].ops = g.grants.a[i].ops;
        n++;
    }
    k_mutex_unlock(&g_lock);
    return n;
}

void akira_seal_revoke(const char *app, akira_seal_op_t op)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    always_t *a = find_always_locked(app);

    if (a) {
        a->ops &= (op == AKIRA_SEAL_NONE) ? 0 : (uint16_t)~BIT(op);
        if (a->ops == 0) {
            int idx = a - g.grants.a;

            memmove(a, a + 1, sizeof(*a) * (g.grants.n - idx - 1));
            g.grants.n--;
        }
        grants_save_locked();
    }
    run_t *r = find_run_locked(app, false);

    if (r) {
        r->live_always &= (op == AKIRA_SEAL_NONE) ? 0 : (uint16_t)~BIT(op);
    }
    k_mutex_unlock(&g_lock);
    LOG_INF("Seal: revoked %s for %s", op == AKIRA_SEAL_NONE ? "everything" :
            akira_seal_op_title(op), app);
}

/* ---- secure element ------------------------------------------------------- */

#if defined(CONFIG_AKIRA_SE050)
static const struct device *seal_se(void)
{
    const struct device *se = se050_get_device();

    return (se && device_is_ready(se)) ? se : NULL;
}
#endif

bool akira_seal_has_secure_element(void)
{
#if defined(CONFIG_AKIRA_SE050)
    if (!g_have_se_known) {
        const struct device *se = seal_se();

        g.have_se = (se && se050_ecc_gen_key(se, CONFIG_AKIRA_CRYSTAL_SE050_KEY_OBJID) == 0)
                        ? 1 : 0;
        g_have_se_known = true;
    }
    return g.have_se == 1;
#else
    return false;
#endif
}

int akira_seal_pubkey(uint8_t pub[65])
{
#if defined(CONFIG_AKIRA_SE050)
    if (!akira_seal_has_secure_element()) {
        return -ENODEV;
    }
    return se050_ecc_get_pub(seal_se(), CONFIG_AKIRA_CRYSTAL_SE050_KEY_OBJID, pub);
#else
    ARG_UNUSED(pub);
    return -ENODEV;
#endif
}

/* ---- ledger ---------------------------------------------------------------- */

static int ledger_entries(const char *path)
{
    struct fs_dirent ent;

    if (fs_stat(path, &ent) != 0) {
        return 0;
    }
    return (int)(ent.size / sizeof(akira_seal_entry_t));
}

int akira_seal_ledger_count(void)
{
    k_mutex_lock(&g_ledger_lock, K_FOREVER);
    int n = ledger_entries(LEDGER_PATH);

    k_mutex_unlock(&g_ledger_lock);
    return n;
}

int akira_seal_ledger_read(int index_from_newest, akira_seal_entry_t *out)
{
    struct fs_file_t f;
    int ret;

    k_mutex_lock(&g_ledger_lock, K_FOREVER);
    int n = ledger_entries(LEDGER_PATH);

    if (index_from_newest < 0 || index_from_newest >= n) {
        k_mutex_unlock(&g_ledger_lock);
        return -ENOENT;
    }
    fs_file_t_init(&f);
    ret = fs_open(&f, LEDGER_PATH, FS_O_READ);
    if (ret == 0) {
        off_t off = (off_t)(n - 1 - index_from_newest) * sizeof(*out);

        ret = fs_seek(&f, off, FS_SEEK_SET);
        if (ret == 0) {
            ret = fs_read(&f, out, sizeof(*out)) == (ssize_t)sizeof(*out) ? 0 : -EIO;
        }
        fs_close(&f);
    }
    k_mutex_unlock(&g_ledger_lock);
    return ret;
}

void akira_seal_sigil_seed(const akira_seal_entry_t *e, uint8_t seed[16])
{
    uint8_t h[32];
    mbedtls_sha256_context c;

    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, (const uint8_t *)&e->rec, sizeof(e->rec));
    mbedtls_sha256_update(&c, e->sig, sizeof(e->sig));
    mbedtls_sha256_finish(&c, h);
    mbedtls_sha256_free(&c);
    memcpy(seed, h, 16);
}

/* Build, sign and append one entry; fills @p out.  Any thread. */
static int ledger_commit(akira_seal_op_t op, akira_seal_scope_t scope, const char *app,
                         uint32_t app_id, const char *detail, akira_seal_entry_t *out)
{
    uint8_t digest[32];

    k_mutex_lock(&g_ledger_lock, K_FOREVER);
    memset(out, 0, sizeof(*out));

    akira_seal_record_t *r = &out->rec;
    uint32_t counter;

    akira_crystal_get_ledger_tip(&counter, r->prev);
    r->magic = SEAL_MAGIC;
    r->version = 1;
    r->op = (uint8_t)op;
    r->scope = (uint8_t)scope;
    r->counter = counter + 1;
    r->epoch = akira_time_is_set() ? akira_time_get_epoch() : 0;
    r->uptime_s = (uint32_t)(k_uptime_get() / 1000);
    r->app_id = app_id;
    strncpy(r->app, app ? app : "?", sizeof(r->app) - 1);
    strncpy(r->detail, detail ? detail : "", sizeof(r->detail) - 1);

    mbedtls_sha256((const uint8_t *)r, sizeof(*r), digest, 0);
#if defined(CONFIG_AKIRA_SE050)
    if (akira_seal_has_secure_element() &&
        se050_ecc_sign(seal_se(), CONFIG_AKIRA_CRYSTAL_SE050_KEY_OBJID, digest, out->sig) == 0) {
        out->signed_by_se = 1;
    } else if (akira_seal_has_secure_element()) {
        LOG_WRN("Seal: SE050 signing failed — entry %u is unsigned", r->counter);
    }
#endif

    struct fs_dirent ent;

    if (fs_stat(DIR_PATH, &ent) != 0) {
        fs_mkdir(DIR_PATH);
    }
    if (ledger_entries(LEDGER_PATH) >= LEDGER_ROTATE) {
        fs_unlink(LEDGER_OLD_PATH);
        fs_rename(LEDGER_PATH, LEDGER_OLD_PATH);
    }
    ssize_t w = fs_manager_append_file(LEDGER_PATH, out, sizeof(*out));

    if (w != (ssize_t)sizeof(*out)) {
        LOG_ERR("Seal: ledger append failed (%d)", (int)w);
    }
    akira_crystal_set_ledger_tip(r->counter, digest);
    k_mutex_unlock(&g_ledger_lock);

    LOG_INF("Seal #%u: %s %s for %s%s", r->counter, akira_seal_op_title(op),
            scope == AKIRA_SCOPE_ALWAYS ? "ALWAYS" :
            scope == AKIRA_SCOPE_ONCE ? "this time" :
            scope == AKIRA_SCOPE_TIMEOUT ? "timed out" : "refused",
            r->app, out->signed_by_se ? " (signed)" : "");
    return w == (ssize_t)sizeof(*out) ? 0 : -EIO;
}

/* ---- the request path ------------------------------------------------------- */

void akira_seal_set_ui(int (*present)(akira_seal_req_t *req))
{
    g.present = present;
}

static void fill_req(akira_seal_op_t op, const char *app, const char *detail)
{
    memset(&g.req, 0, sizeof(g.req));
    g.req.op = op;
    strncpy(g.req.app, app, sizeof(g.req.app) - 1);
    strncpy(g.req.detail, detail ? detail : "", sizeof(g.req.detail) - 1);
    g.req.allow_always = !akira_seal_op_is_system(op);
    g.req.scope = AKIRA_SCOPE_REFUSED;
    g.abandoned = false;
}

/* Present g.req and wait for the answer (app threads).  Holds g_slot. */
static akira_seal_scope_t ask_and_wait(void)
{
    k_sem_reset(&g_done);
    g.from_shell = false;
    if (!g.present || g.present(&g.req) != 0) {
        return AKIRA_SCOPE_REFUSED;
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    g.waiting = true;
    k_mutex_unlock(&g_lock);
    /* The UI always answers (its own timeout, HOME, app stop), so this only
     * fires if the shell itself is wedged. */
    int ret = k_sem_take(&g_done, K_SECONDS(CONFIG_AKIRA_SEAL_TIMEOUT_S + 60));

    k_mutex_lock(&g_lock, K_FOREVER);
    g.waiting = false;
    if (ret != 0) {
        g.abandoned = true;
    }
    k_mutex_unlock(&g_lock);
    if (ret != 0) {
        LOG_ERR("Seal: UI never answered — refusing");
        return AKIRA_SCOPE_TIMEOUT;
    }
    return g.req.scope;
}

/* After ask_and_wait(): free the slot, unless the request was abandoned —
 * then its seal screen may still be up, and release() frees it on close. */
static void give_slot_unless_abandoned(void)
{
    k_mutex_lock(&g_lock, K_FOREVER);
    bool abandoned = g.abandoned;

    k_mutex_unlock(&g_lock);
    if (!abandoned) {
        k_sem_give(&g_slot);
    }
}

/* Bounded: a wedged UI must not freeze every app that asks after it. */
static int take_slot(void)
{
    return k_sem_take(&g_slot, K_SECONDS(CONFIG_AKIRA_SEAL_TIMEOUT_S + 30)) == 0 ? 0 : -EBUSY;
}

void akira_seal_set_ui_cancel(void (*cancel)(void))
{
    g.cancel_ui = cancel;
}

void akira_seal_abandon_app(const char *app)
{
    bool hit;

    if (!app) {
        return;
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    hit = g.waiting && !g.from_shell && !g.abandoned &&
          strncmp(g.req.app, app, AKIRA_APP_NAME_LEN) == 0;
    if (hit) {
        g.abandoned = true;
        g.req.scope = AKIRA_SCOPE_REFUSED;
    }
    k_mutex_unlock(&g_lock);
    if (hit) {
        LOG_INF("Seal: %s is going away — its request is refused", app);
        k_sem_give(&g_done);  /* wake the app thread now */
        if (g.cancel_ui) {
            g.cancel_ui();    /* and take the seal screen down */
        }
    }
}

int akira_seal_require(const char *app, akira_seal_op_t op, const char *detail)
{
    if (!app || !op_valid(op) || akira_seal_op_is_system(op)) {
        return -EINVAL;
    }
    if (akira_seal_granted(app, op)) {
        return 0;
    }
    if (!g.present || take_slot() != 0) {
        return -EBUSY;
    }
    if (akira_seal_granted(app, op)) { /* granted while we queued */
        k_sem_give(&g_slot);
        return 0;
    }
    fill_req(op, app, detail);

    akira_seal_scope_t scope = ask_and_wait();

    give_slot_unless_abandoned();
    return (scope == AKIRA_SCOPE_ONCE || scope == AKIRA_SCOPE_ALWAYS) ? 0
           : scope == AKIRA_SCOPE_TIMEOUT                            ? -ETIMEDOUT
                                                                     : -EACCES;
}

int akira_seal_record(const char *app, const char *what, int64_t value)
{
    char detail[AKIRA_SEAL_DETAIL_LEN];

    if (!app) {
        return -EINVAL;
    }
    snprintf(detail, sizeof(detail), "%s: %lld", what && what[0] ? what : "Record",
             (long long)value);

    /* ALWAYS for records = seal silently; every record is still its own
     * signed entry. */
    if (akira_seal_granted(app, AKIRA_SEAL_RECORD)) {
        akira_seal_entry_t e;

        akira_crystal_add_use(AKIRA_DOM_PLAY, app, AKIRA_SEAL_RECORD);
        return ledger_commit(AKIRA_SEAL_RECORD, AKIRA_SCOPE_ALWAYS, app,
                             app_manager_identity(app), detail, &e);
    }
    if (!g.present || take_slot() != 0) {
        return -EBUSY;
    }
    fill_req(AKIRA_SEAL_RECORD, app, detail);

    akira_seal_scope_t scope = ask_and_wait();

    give_slot_unless_abandoned();
    return (scope == AKIRA_SCOPE_ONCE || scope == AKIRA_SCOPE_ALWAYS) ? 0 : -EACCES;
}

void akira_seal_ask(akira_seal_op_t op, const char *detail, bool (*should_cancel)(void),
                    void (*on_result)(bool granted, void *ctx), void *ctx)
{
    if (!op_valid(op) || !g.present || k_sem_take(&g_slot, K_NO_WAIT) != 0) {
        if (on_result) {
            on_result(false, ctx);
        }
        return;
    }
    fill_req(op, AKIRA_SEAL_SYSTEM_APP, detail);
    g.req.should_cancel = should_cancel;
    g.from_shell = true;
    g.on_result = on_result;
    g.ctx = ctx;
    if (g.present(&g.req) != 0) {
        g.from_shell = false;
        k_sem_give(&g_slot);
        if (on_result) {
            on_result(false, ctx);
        }
    }
}

void akira_seal_finish(akira_seal_req_t *req, akira_seal_scope_t scope)
{
    bool granted = (scope == AKIRA_SCOPE_ONCE || scope == AKIRA_SCOPE_ALWAYS);

    if (scope == AKIRA_SCOPE_ALWAYS && !req->allow_always) {
        scope = AKIRA_SCOPE_ONCE;
    }
    req->scope = scope;
    req->new_shard = false;

    uint32_t app_id = 0;

    if (granted && !g.from_shell) {
        app_id = app_manager_identity(req->app); /* before g_lock (lock order) */
    }

    if (granted && !g.from_shell && req->op != AKIRA_SEAL_RECORD) {
        k_mutex_lock(&g_lock, K_FOREVER);
        run_t *r = find_run_locked(req->app, true);

        r->once |= BIT(req->op);
        if (scope == AKIRA_SCOPE_ALWAYS) {
            always_t *a = find_always_locked(req->app);

            if (!a && g.grants.n < MAX_ALWAYS) {
                a = &g.grants.a[g.grants.n++];
                memset(a, 0, sizeof(*a));
                strncpy(a->app, req->app, sizeof(a->app) - 1);
            }
            if (a) {
                if (a->id != app_id) {
                    a->ops = 0; /* a different build: start over */
                }
                a->id = app_id;
                a->ops |= BIT(req->op);
                grants_save_locked();
            }
            r->live_always |= BIT(req->op);
        }
        k_mutex_unlock(&g_lock);
    } else if (granted && !g.from_shell && req->op == AKIRA_SEAL_RECORD &&
               scope == AKIRA_SCOPE_ALWAYS) {
        k_mutex_lock(&g_lock, K_FOREVER);
        always_t *a = find_always_locked(req->app);

        if (!a && g.grants.n < MAX_ALWAYS) {
            a = &g.grants.a[g.grants.n++];
            memset(a, 0, sizeof(*a));
            strncpy(a->app, req->app, sizeof(a->app) - 1);
        }
        if (a) {
            if (a->id != app_id) {
                a->ops = 0;
            }
            a->id = app_id;
            a->ops |= BIT(AKIRA_SEAL_RECORD);
            grants_save_locked();
        }
        k_mutex_unlock(&g_lock);
    }

    /* Every answer is on the record — refusals too — but only grants grow. */
    ledger_commit(req->op, scope, req->app, app_id, req->detail, &req->entry);
    if (granted) {
        req->new_shard = akira_crystal_add_use(akira_seal_op_domain(req->op), req->app,
                                               req->op);
    }
    akira_crystal_flush();
}

void akira_seal_release(akira_seal_req_t *req)
{
    bool granted = (req->scope == AKIRA_SCOPE_ONCE || req->scope == AKIRA_SCOPE_ALWAYS);

    if (g.from_shell) {
        void (*cb)(bool, void *) = g.on_result;
        void *ctx = g.ctx;

        g.from_shell = false;
        g.on_result = NULL;
        k_sem_give(&g_slot);
        if (cb) {
            cb(granted, ctx);
        }
        return;
    }
    k_mutex_lock(&g_lock, K_FOREVER);
    bool abandoned = g.abandoned;

    g.abandoned = false;
    k_mutex_unlock(&g_lock);
    if (abandoned) {
        k_sem_give(&g_slot); /* the asker already left; the screen is down now */
    } else {
        k_sem_give(&g_done); /* the app thread frees the slot */
    }
}
