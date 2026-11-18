/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_crystal_cmds.c
 * @brief `crystal` shell commands: inspect Akira, audit the ledger, test seals.
 *
 * (Not `akira`: that name is already taken by the runtime commands.)
 */

#include "akira_seal.h"

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <runtime/security.h>

static const char *scope_name(uint8_t s)
{
    switch (s) {
    case AKIRA_SCOPE_ONCE:    return "this time";
    case AKIRA_SCOPE_ALWAYS:  return "always";
    case AKIRA_SCOPE_TIMEOUT: return "timed out";
    default:                  return "refused";
    }
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
    akira_crystal_view_t v;

    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    akira_crystal_get(&v);
    shell_print(sh, "Akira — day %u, %u seals, %u glints, %u cracks%s", v.days, v.seals,
                v.glints, v.n_cracks, v.loaded ? "" : " (not loaded)");
    for (int d = 0; d < AKIRA_DOM_COUNT; d++) {
        shell_print(sh, "  %-6s %u shard(s), %u use(s)", akira_domain_name(d), v.shards[d],
                    v.uses[d]);
    }
    shell_print(sh, "  seed %02x%02x%02x%02x%02x%02x%02x%02x, seals %s", v.seed[0], v.seed[1],
                v.seed[2], v.seed[3], v.seed[4], v.seed[5], v.seed[6], v.seed[7],
                akira_seal_has_secure_element() ? "signed by SE050" : "unsigned (no SE050)");

    akira_seal_grant_t g[16];
    int n = akira_seal_list_grants(g, ARRAY_SIZE(g));

    for (int i = 0; i < n; i++) {
        for (int op = 1; op < AKIRA_SEAL_OP_COUNT; op++) {
            if (g[i].ops & BIT(op)) {
                shell_print(sh, "  kept: %s — %s", g[i].app, akira_seal_op_title(op));
            }
        }
    }
    return 0;
}

static int cmd_ledger(const struct shell *sh, size_t argc, char **argv)
{
    int want = argc > 1 ? atoi(argv[1]) : 10;
    int n = akira_seal_ledger_count();
    akira_seal_entry_t e;

    shell_print(sh, "%d entr%s in the current ledger", n, n == 1 ? "y" : "ies");
    for (int i = 0; i < MIN(want, n); i++) {
        if (akira_seal_ledger_read(i, &e) != 0) {
            break;
        }
        shell_print(sh, "#%-4u %-24s %-10s %-16s %s%s", e.rec.counter,
                    akira_seal_op_title(e.rec.op), scope_name(e.rec.scope), e.rec.app,
                    e.rec.detail, e.signed_by_se ? "" : "  [unsigned]");
    }
    return 0;
}

static int cmd_pubkey(const struct shell *sh, size_t argc, char **argv)
{
    uint8_t pub[65];
    char hex[131];

    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    if (akira_seal_pubkey(pub) != 0) {
        shell_error(sh, "No secure element: seals are unsigned on this console");
        return -ENODEV;
    }
    for (int i = 0; i < 65; i++) {
        snprintk(&hex[i * 2], 3, "%02x", pub[i]);
    }
    shell_print(sh, "Seal key (P-256, uncompressed): %s", hex);
    return 0;
}

static void ask_done(bool granted, void *ctx)
{
    ARG_UNUSED(ctx);
    printk("crystal: seal %s\n", granted ? "granted" : "refused");
}

static int cmd_seal(const struct shell *sh, size_t argc, char **argv)
{
    int op = argc > 1 ? atoi(argv[1]) : AKIRA_SEAL_APP_INSTALL;

    if (op <= AKIRA_SEAL_NONE || op >= AKIRA_SEAL_OP_COUNT) {
        shell_error(sh, "op 1..%d", AKIRA_SEAL_OP_COUNT - 1);
        return -EINVAL;
    }
    /* Asks as the OS, so the prompt shows THIS TIME only; real powers are
     * asked for by the apps that use them. */
    akira_seal_ask((akira_seal_op_t)op, argc > 2 ? argv[2] : "Shell test", NULL, ask_done,
                   NULL);
    shell_print(sh, "Asked: %s — answer on the console", akira_seal_op_title(op));
    return 0;
}

static int cmd_mend(const struct shell *sh, size_t argc, char **argv)
{
    akira_crystal_mend(argc > 1 ? atoi(argv[1]) : -1);
    shell_print(sh, "Mended");
    return 0;
}

static int cmd_revoke(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: crystal revoke <app> [op]");
        return -EINVAL;
    }
    akira_seal_revoke(argv[1], argc > 2 ? (akira_seal_op_t)atoi(argv[2]) : AKIRA_SEAL_NONE);
    shell_print(sh, "Revoked");
    return 0;
}

#if defined(CONFIG_AKIRA_CRYSTAL_DEBUG)
static int cmd_grow(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: crystal grow <domain 0-5> [shards] [uses]");
        return -EINVAL;
    }
    akira_crystal_debug_grow((akira_domain_t)atoi(argv[1]), argc > 2 ? atoi(argv[2]) : 1,
                             argc > 3 ? atoi(argv[3]) : 4);
    return cmd_status(sh, 1, argv);
}

static int cmd_day(const struct shell *sh, size_t argc, char **argv)
{
    akira_crystal_debug_days(argc > 1 ? atoi(argv[1]) : 1);
    return cmd_status(sh, 1, argv);
}

static int cmd_glints(const struct shell *sh, size_t argc, char **argv)
{
    akira_crystal_debug_glints(argc > 1 ? atoi(argv[1]) : 1);
    return cmd_status(sh, 1, argv);
}

static int cmd_crack(const struct shell *sh, size_t argc, char **argv)
{
    akira_crystal_note_denial(argc > 1 ? argv[1] : "test_app",
                              argc > 2 ? strtoull(argv[2], NULL, 0) : AKIRA_CAP_RF_TRANSCEIVE);
    return cmd_status(sh, 1, argv);
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
    akira_crystal_debug_reset();
    return cmd_status(sh, 1, argv);
}

/* Ask as an app would — from its own thread, blocking until answered. */
static K_THREAD_STACK_DEFINE(ask_stack, 2048);
static struct k_thread ask_thread;
static char ask_app[AKIRA_APP_NAME_LEN];
static int ask_op;

static void ask_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    int ret = (ask_op == AKIRA_SEAL_RECORD)
                  ? akira_seal_record(ask_app, "High score", 12480)
                  : akira_seal_require(ask_app, (akira_seal_op_t)ask_op, "debug request");

    printk("crystal: %s asked for %s -> %d\n", ask_app, akira_seal_op_title(ask_op), ret);
}

static int cmd_ask(const struct shell *sh, size_t argc, char **argv)
{
    ask_op = atoi(argv[1]);
    snprintk(ask_app, sizeof(ask_app), "%s", argc > 2 ? argv[2] : "test_app");
    k_thread_create(&ask_thread, ask_stack, K_THREAD_STACK_SIZEOF(ask_stack), ask_fn, NULL,
                    NULL, NULL, 12, 0, K_NO_WAIT);
    shell_print(sh, "%s asks for %s", ask_app, akira_seal_op_title(ask_op));
    return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(crystal_cmds,
    SHELL_CMD(status, NULL, "Akira's growth, cracks and kept seals", cmd_status),
    SHELL_CMD_ARG(ledger, NULL, "ledger [n] — newest seals", cmd_ledger, 1, 1),
    SHELL_CMD(pubkey, NULL, "Seal key public key (verify signatures)", cmd_pubkey),
    SHELL_CMD_ARG(seal, NULL, "seal [op] [detail] — test the Seal prompt", cmd_seal, 1, 2),
    SHELL_CMD_ARG(mend, NULL, "mend [index] — mend cracks (all by default)", cmd_mend, 1, 1),
    SHELL_CMD_ARG(revoke, NULL, "revoke <app> [op] — drop ALWAYS grants", cmd_revoke, 2, 1),
#if defined(CONFIG_AKIRA_CRYSTAL_DEBUG)
    SHELL_CMD_ARG(grow, NULL, "grow <domain> [shards] [uses] (debug)", cmd_grow, 2, 2),
    SHELL_CMD_ARG(day, NULL, "day [n] (debug)", cmd_day, 1, 1),
    SHELL_CMD_ARG(glints, NULL, "glints [n] (debug)", cmd_glints, 1, 1),
    SHELL_CMD_ARG(crack, NULL, "crack [app] [cap] (debug)", cmd_crack, 1, 2),
    SHELL_CMD(reset, NULL, "forget growth, keep the seed (debug)", cmd_reset),
    SHELL_CMD_ARG(ask, NULL, "ask <op> [app] — request as an app thread (debug)", cmd_ask, 2, 1),
#endif
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(crystal, &crystal_cmds, "Akira — the crystal and the Seal", NULL);
