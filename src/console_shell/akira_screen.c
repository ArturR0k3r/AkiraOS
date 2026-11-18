/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_akira_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_akira_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_screen.c
 * @brief Akira, inspected (see akira_screen.h).
 *
 * Main page (SCR_W x 240, dark, full-bleed):
 *   left    Akira, slowly turning; LEFT / RIGHT spin it
 *   right   name, day and seal count; one row per domain; glints
 *   bottom  Ledger · Kept seals · Cracks — UP / DOWN, A opens, B back
 *
 * Only the crystal's box is presented while it turns.  The list pages and
 * the ledger detail share the same dark look.
 */

#include "akira_screen.h"
#include "home_screen.h"
#include "shell_theme.h"
#include "ui/akira_logo.h"
#include "ui/akira_ui.h"
#include "ui/anim.h"
#include "ui/crystal_render.h"
#include "ui/overlay.h"
#include "ui/screen.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <crystal/akira_seal.h>
#include <drivers/platform_hal.h>
#include <lib/akira_time.h>
#include <lib/mem_helper.h>
#include <runtime/app_manager/app_manager.h>
#include <runtime/security.h>
#ifdef CONFIG_AKIRA_SETTINGS
#include <settings/settings.h>
#endif

/* ---- palette ------------------------------------------------------------ */

static bool mono(void)
{
    return akira_logo_panel_is_mono();
}

static uint16_t bg(void)
{
    return mono() ? 0xFFFFu : AKIRA_BOOT_SPLASH_BG;
}

static uint16_t ink(void)
{
    return mono() ? 0x0000u : 0xFFFFu;
}

static uint16_t dim(void)
{
    return mono() ? 0x0000u : 0x8C97u;
}

static uint16_t faint(void)
{
    return mono() ? 0x0000u : 0x4A50u; /* well below dim, for empty rows */
}

static uint16_t panel(void)
{
    return mono() ? 0xFFFFu : 0x10A6u; /* #121436 — a raised navy */
}

static uint16_t dom_colour(int d)
{
    return mono() ? 0x0000u : akira_domain_colour(d);
}

static uint16_t warn(void)
{
    return mono() ? 0x0000u : 0xFD48u; /* warm amber-red, for cracks */
}

static void diamond(int x, int y, int r, uint16_t c)
{
    akira_display_triangle_fill(x - r, y, x, y - r, x + r, y, c);
    akira_display_triangle_fill(x - r, y, x, y + r, x + r, y, c);
}

static void text_right(int xr, int y, const char *s, uint16_t c)
{
    akira_display_text(xr - akira_ui_text_w(s, AKIRA_UI_FONT_SMALL), y, s, c);
}

/* Left text that stops short of a right-aligned tag on the same line. */
static void text_left_fit(int x, int y, char *s, const char *right_tag, uint16_t c)
{
    int room = (SCR_W - 16 - akira_ui_text_w(right_tag, AKIRA_UI_FONT_SMALL) - 8 - x) / 8;
    int n = (int)strlen(s);

    if (room > 2 && n > room) {
        s[room - 2] = '.';
        s[room - 1] = '.';
        s[room] = '\0';
    }
    akira_display_text(x, y, s, c);
}

/* "Nov 18 14:02", or uptime when the clock was never set. */
static void when(int64_t epoch, uint32_t uptime_s, char *buf, size_t len)
{
    static const char *const mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    if (epoch <= 0) {
        snprintf(buf, len, "up %uh%02um", uptime_s / 3600, (uptime_s / 60) % 60);
        return;
    }
    int64_t t = epoch + akira_time_get_tz_offset_s();
    int64_t days = t / 86400;
    int rem = (int)(t % 86400);

    /* civil_from_days (Howard Hinnant) */
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;

    snprintf(buf, len, "%s %u %02d:%02d", mon[m - 1], d, rem / 3600, (rem / 60) % 60);
}

static const char *scope_name(uint8_t s)
{
    switch (s) {
    case AKIRA_SCOPE_ONCE:    return "this time";
    case AKIRA_SCOPE_ALWAYS:  return "always";
    case AKIRA_SCOPE_TIMEOUT: return "timed out";
    default:                  return "refused";
    }
}

/* ======================================================================== */
/* Main page                                                                 */
/* ======================================================================== */

#define CRY_X 86
#define CRY_Y 106
#define CRY_R 60
#define CRY_BOX (CRY_R * 118 / 100 + 6)
#define PANEL_X 170
#define MENU_Y 150

/* List modes (also the list screen's mode values). */
enum { M_LEDGER, M_KEPT, M_CRACKS };
/* Main-page menu, top to bottom. */
enum { MI_HOW, MI_LEDGER, MI_KEPT, MI_CRACKS, MI_COUNT };

static struct {
    akira_crystal_view_t v;
    int sel;
    int64_t t0;
    anim_t spin;      /* extra rotation from LEFT / RIGHT */
    int64_t last_load;
    int kept;         /* number of always-grants, for the menu label */
} g_main AKIRA_BULK_BSS;

static const shell_screen_t list_scr;
static void list_open(int mode);

static void main_reload(void)
{
    akira_seal_grant_t gr[16];
    int n = akira_seal_list_grants(gr, ARRAY_SIZE(gr));

    akira_crystal_get(&g_main.v);
    g_main.kept = 0;
    for (int i = 0; i < n; i++) {
        g_main.kept += __builtin_popcount(gr[i].ops);
    }
    g_main.last_load = k_uptime_get();
}

/* One fact on the right: a coloured mark, a line, and what it means. */
static void fact(int y, uint16_t mark, const char *line, const char *meaning, uint16_t ink_c)
{
    diamond(PANEL_X + 4, y + 5, 4, mark);
    akira_display_text(PANEL_X + 14, y, line, ink_c);
    akira_display_text(PANEL_X + 14, y + 12, meaning, faint());
}

static void main_draw(void)
{
    char buf[40];
    const akira_crystal_view_t *v = &g_main.v;
    int64_t t = k_uptime_get() - g_main.t0;
    uint32_t allowed = 0;

    for (int d = 0; d < AKIRA_DOM_COUNT; d++) {
        allowed += v->shards[d];
    }
    akira_display_clear(bg());

    /* Akira, turning: a full turn every 40 s, plus whatever spin was given. */
    if (!mono()) {
        akira_logo_glow(CRY_X, CRY_Y, CRY_R + 14, 0x29D3u, 150);
    }
    crystal_fx_t fx;

    crystal_fx_init(&fx, CRY_X, CRY_Y, CRY_R, bg());
    fx.angle = (int)((t * CRYSTAL_TURN / 40000) % CRYSTAL_TURN) + anim_value(&g_main.spin);
    crystal_draw(v, &fx);

    /* Who she is, in a sentence. */
    akira_display_text_large(PANEL_X, 10, "Akira", ink());
    snprintf(buf, sizeof(buf), "day %u with you", MAX(v->days, 1));
    akira_display_text(PANEL_X, 32, buf, dim());

    /* Three facts, each with what it means. */
    snprintf(buf, sizeof(buf), "%u power%s allowed", allowed, allowed == 1 ? "" : "s");
    fact(52, dom_colour(AKIRA_DOM_RADIO), buf, "each is a shard", ink());
    snprintf(buf, sizeof(buf), "%u glint%s", v->glints, v->glints == 1 ? "" : "s");
    fact(80, dom_colour(AKIRA_DOM_PLAY), buf, "won in games", ink());
    if (v->n_cracks) {
        snprintf(buf, sizeof(buf), "%u crack%s", v->n_cracks, v->n_cracks == 1 ? "" : "s");
        fact(108, warn(), buf, "an app blocked", warn());
    } else {
        fact(108, faint(), "no cracks", "all clear", dim());
    }

    /* Menu. */
    for (int i = 0; i < MI_COUNT; i++) {
        int y = MENU_Y + i * 15;
        bool hi = (i == g_main.sel);

        switch (i) {
        case MI_HOW:    snprintf(buf, sizeof(buf), "How it works"); break;
        case MI_LEDGER: snprintf(buf, sizeof(buf), "Seal history (%u)", v->seals); break;
        case MI_KEPT:   snprintf(buf, sizeof(buf), "Always allowed (%d)", g_main.kept); break;
        default:
            snprintf(buf, sizeof(buf), v->n_cracks ? "Cracks (%u)" : "Cracks", v->n_cracks);
            break;
        }
        if (hi) {
            akira_display_rounded_rect_fill(PANEL_X - 4, y - 3, SCR_W - PANEL_X - 4, 15, 4,
                                            mono() ? 0x0000u : panel());
        }
        uint16_t c = (i == MI_CRACKS && v->n_cracks) ? warn() : (hi ? ink() : dim());

        if (mono() && hi) {
            c = 0xFFFFu;
        }
        akira_display_text(PANEL_X + 6, y, buf, c);
        if (hi) {
            akira_display_text(SCR_W - 18, y, ">", c);
        }
    }
    akira_display_text(10, 226, "< > turn Akira", faint());
    text_right(SCR_W - 10, 226, "A open   B back", faint());
}

static bool main_tick(void)
{
    /* Growth can change behind us (an app sealed something): re-read now
     * and then; the crystal's box is presented every frame anyway. */
    if (k_uptime_get() - g_main.last_load > 2000) {
        akira_crystal_view_t old = g_main.v;
        int old_kept = g_main.kept;

        main_reload();
        if (memcmp(&old, &g_main.v, offsetof(akira_crystal_view_t, loaded)) != 0 ||
            old_kept != g_main.kept) {
            return true;
        }
    }
    screen_invalidate_rect(CRY_X - CRY_BOX, CRY_Y - CRY_BOX, CRY_BOX * 2, CRY_BOX * 2);
    return false;
}

static void main_key(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_B)) {
        screen_pop();
        return;
    }
    if (just & (BIT(AKIRA_BTN_LEFT) | BIT(AKIRA_BTN_RIGHT))) {
        int dir = (just & BIT(AKIRA_BTN_LEFT)) ? -1 : 1;

        anim_to(&g_main.spin, anim_value(&g_main.spin) + dir * CRYSTAL_TURN / 6, 700,
                ANIM_EASE_OUT_BACK);
    }
    if (just & BIT(AKIRA_BTN_UP)) {
        g_main.sel = (g_main.sel + MI_COUNT - 1) % MI_COUNT;
        screen_invalidate();
    }
    if (just & BIT(AKIRA_BTN_DOWN)) {
        g_main.sel = (g_main.sel + 1) % MI_COUNT;
        screen_invalidate();
    }
    if (just & BIT(AKIRA_BTN_A)) {
        switch (g_main.sel) {
        case MI_HOW:    akira_intro_open(false); break;
        case MI_LEDGER: list_open(M_LEDGER); break;
        case MI_KEPT:   list_open(M_KEPT); break;
        default:        list_open(M_CRACKS); break;
        }
    }
}

static void main_enter(void *arg)
{
    ARG_UNUSED(arg);
    memset(&g_main, 0, sizeof(g_main));
    g_main.t0 = k_uptime_get();
    anim_set(&g_main.spin, 0);
    main_reload();
    /* Cracks waiting? Put the cursor on them. */
    if (g_main.v.n_cracks) {
        g_main.sel = MI_CRACKS;
    }
}

static void main_resume(void)
{
    main_reload();
}

static const shell_screen_t main_scr = {
    .name = "akira",
    .flags = 0,
    .enter = main_enter,
    .resume = main_resume,
    .on_key = main_key,
    .tick = main_tick,
    .draw = main_draw,
};

void akira_screen_open(void)
{
    akira_crystal_init();
    screen_push(&main_scr, NULL);
}

void akira_screen_status(char *buf, size_t len)
{
    akira_crystal_view_t v;

    akira_crystal_get(&v);
    if (v.n_cracks) {
        snprintf(buf, len, "%u crack%s", v.n_cracks, v.n_cracks == 1 ? "" : "s");
    } else {
        snprintf(buf, len, "Day %u", MAX(v.days, 1));
    }
}

/* ======================================================================== */
/* Lists: ledger, kept seals, cracks                                         */
/* ======================================================================== */

#define LIST_MAX 64
#define ROW_H 34
#define ROWS_Y 42
#define ROWS_VIS 5

typedef struct {
    akira_seal_grant_t grant;
    uint8_t op;
} kept_t;

static struct {
    int mode;
    int sel, scroll, n;
    union {
        akira_seal_entry_t ledger[LIST_MAX];
        kept_t kept[LIST_MAX];
        akira_crack_t cracks[AKIRA_MAX_CRACKS];
    };
} g_list AKIRA_BULK_BSS;

static void list_load(void)
{
    g_list.n = 0;
    if (g_list.mode == M_LEDGER) {
        int total = akira_seal_ledger_count();

        for (int i = 0; i < MIN(total, LIST_MAX); i++) {
            if (akira_seal_ledger_read(i, &g_list.ledger[g_list.n]) == 0) {
                g_list.n++;
            }
        }
    } else if (g_list.mode == M_KEPT) {
        akira_seal_grant_t gr[16];
        int n = akira_seal_list_grants(gr, ARRAY_SIZE(gr));

        for (int i = 0; i < n; i++) {
            for (int op = 1; op < AKIRA_SEAL_OP_COUNT && g_list.n < LIST_MAX; op++) {
                if (gr[i].ops & BIT(op)) {
                    g_list.kept[g_list.n].grant = gr[i];
                    g_list.kept[g_list.n].op = (uint8_t)op;
                    g_list.n++;
                }
            }
        }
    } else {
        g_list.n = akira_crystal_get_cracks(g_list.cracks, AKIRA_MAX_CRACKS);
    }
    g_list.sel = CLAMP(g_list.sel, 0, MAX(g_list.n - 1, 0));
    g_list.scroll = CLAMP(g_list.scroll, 0, MAX(g_list.n - ROWS_VIS, 0));
}

static const char *list_title(void)
{
    return g_list.mode == M_LEDGER ? "Seal history" : g_list.mode == M_KEPT ? "Always allowed"
                                                                              : "Cracks";
}

static void list_row(int i, int y, bool hi)
{
    char l1[48], l2[64], w[24];

    if (hi) {
        akira_display_rounded_rect_fill(8, y + 1, SCR_W - 16, ROW_H - 3, 6, panel());
        if (mono()) {
            akira_display_rounded_rect(8, y + 1, SCR_W - 16, ROW_H - 3, 6, 0x0000u);
        }
    }
    if (g_list.mode == M_LEDGER) {
        const akira_seal_entry_t *e = &g_list.ledger[i];
        int d = akira_seal_op_domain(e->rec.op);
        uint8_t seed[16];
        bool ok = e->rec.scope == AKIRA_SCOPE_ONCE || e->rec.scope == AKIRA_SCOPE_ALWAYS;

        akira_seal_sigil_seed(e, seed);
        crystal_draw_sigil(30, y + ROW_H / 2, 12, seed, ok ? dom_colour(d) : faint());
        /* a record's title is the record itself */
        snprintf(l1, sizeof(l1), "%.26s", e->rec.op == AKIRA_SEAL_RECORD && e->rec.detail[0]
                                              ? e->rec.detail : akira_seal_op_title(e->rec.op));
        when(e->rec.epoch, e->rec.uptime_s, w, sizeof(w));
        snprintf(l2, sizeof(l2), "%.12s - %s", e->rec.app, scope_name(e->rec.scope));
        text_left_fit(50, y + 19, l2, w, dim());
        text_right(SCR_W - 16, y + 19, w, faint());
        snprintf(w, sizeof(w), "#%u", e->rec.counter);
        text_left_fit(50, y + 5, l1, w, ok ? ink() : dim());
        text_right(SCR_W - 16, y + 5, w, faint());
    } else if (g_list.mode == M_KEPT) {
        const kept_t *k = &g_list.kept[i];

        diamond(28, y + ROW_H / 2, 6, dom_colour(akira_seal_op_domain(k->op)));
        akira_display_text(50, y + 5, akira_seal_op_title(k->op), ink());
        snprintf(l2, sizeof(l2), "%s - always", k->grant.app);
        akira_display_text(50, y + 19, l2, dim());
    } else {
        const akira_crack_t *c = &g_list.cracks[i];

        /* a little zigzag */
        akira_display_line(22, y + 9, 28, y + 15, warn());
        akira_display_line(28, y + 15, 24, y + 19, warn());
        akira_display_line(24, y + 19, 32, y + 26, warn());
        snprintf(l1, sizeof(l1), "%.12s tried %s", c->app, akira_capability_mask_to_str(c->cap));
        when(c->epoch, c->uptime_s, w, sizeof(w));
        snprintf(l2, sizeof(l2), "%s - %u attempt%s", akira_domain_name(c->domain), c->count,
                 c->count == 1 ? "" : "s");
        akira_display_text(50, y + 5, l1, ink());
        text_left_fit(50, y + 19, l2, w, dim());
        text_right(SCR_W - 16, y + 19, w, faint());
    }
}

static void list_draw(void)
{
    char buf[24];

    akira_display_clear(bg());
    akira_display_text_large(14, 12, list_title(), ink());
    snprintf(buf, sizeof(buf), "%d", g_list.n);
    text_right(SCR_W - 14, 16, buf, dim());

    if (g_list.n == 0) {
        const char *empty = g_list.mode == M_LEDGER ? "No seals yet."
                            : g_list.mode == M_KEPT ? "Nothing is always allowed."
                                                    : "No cracks. Nothing was blocked.";

        akira_display_text((SCR_W - akira_ui_text_w(empty, 0)) / 2, 112, empty, dim());
    }
    for (int r = 0; r < ROWS_VIS; r++) {
        int i = g_list.scroll + r;

        if (i >= g_list.n) {
            break;
        }
        list_row(i, ROWS_Y + r * ROW_H, i == g_list.sel);
    }
    const char *hint = g_list.mode == M_LEDGER ? "A details   B back"
                       : g_list.mode == M_KEPT ? "A stop allowing   B back"
                                               : "A mend   X remove app   B back";

    if (g_list.n == 0) {
        hint = "B back";
    }
    text_right(SCR_W - 10, 226, hint, faint());
}

static const shell_screen_t detail_scr;

static void uninstall_done(bool confirmed, void *ctx)
{
    const char *app = ctx;

    if (!confirmed) {
        return;
    }
    if (app_manager_uninstall(app) == 0) {
        akira_crystal_mend(g_list.sel);
        overlay_toast(TOAST_ICON_OK, "Removed", app, 2000);
        home_screen_refresh();
    } else {
        overlay_toast(TOAST_ICON_ERROR, "Couldn't remove", app, 2500);
    }
    list_load();
    screen_invalidate();
}

static void list_key(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_B)) {
        screen_pop();
        return;
    }
    if (g_list.n == 0) {
        return;
    }
    if (just & BIT(AKIRA_BTN_UP) && g_list.sel > 0) {
        g_list.sel--;
    }
    if (just & BIT(AKIRA_BTN_DOWN) && g_list.sel < g_list.n - 1) {
        g_list.sel++;
    }
    if (g_list.sel < g_list.scroll) {
        g_list.scroll = g_list.sel;
    }
    if (g_list.sel >= g_list.scroll + ROWS_VIS) {
        g_list.scroll = g_list.sel - ROWS_VIS + 1;
    }
    if (just & BIT(AKIRA_BTN_A)) {
        if (g_list.mode == M_LEDGER) {
            screen_push(&detail_scr, &g_list.ledger[g_list.sel]);
            return;
        }
        if (g_list.mode == M_KEPT) {
            const kept_t *k = &g_list.kept[g_list.sel];

            akira_seal_revoke(k->grant.app, (akira_seal_op_t)k->op);
            overlay_toast(TOAST_ICON_OK, "It will ask again", akira_seal_op_title(k->op), 2000);
        } else {
            overlay_toast(TOAST_ICON_OK, "Mended", g_list.cracks[g_list.sel].app, 1800);
            akira_crystal_mend(g_list.sel);
        }
        list_load();
    }
    if ((just & BIT(AKIRA_BTN_X)) && g_list.mode == M_CRACKS) {
        static char q[48] AKIRA_BULK_BSS;
        static char app[AKIRA_APP_NAME_LEN] AKIRA_BULK_BSS;

        snprintf(app, sizeof(app), "%s", g_list.cracks[g_list.sel].app);
        snprintf(q, sizeof(q), "Remove %s?", app);
        akira_ui_confirm_open("APP_UNINSTALL", q, NULL, uninstall_done, app);
        return;
    }
    screen_invalidate();
}

static void list_resume(void)
{
    list_load();
}

static const shell_screen_t list_scr = {
    .name = "akira_list",
    .flags = 0,
    .on_key = list_key,
    .resume = list_resume,
    .draw = list_draw,
};

static void list_open(int mode)
{
    memset(&g_list, 0, sizeof(g_list));
    g_list.mode = mode;
    list_load();
    screen_push(&list_scr, NULL);
}

/* ======================================================================== */
/* Ledger entry detail                                                       */
/* ======================================================================== */

static const akira_seal_entry_t *g_detail;

static void detail_enter(void *arg)
{
    g_detail = arg;
}

static void detail_draw(void)
{
    const akira_seal_entry_t *e = g_detail;
    const akira_seal_record_t *r = &e->rec;
    int d = akira_seal_op_domain(r->op);
    uint8_t seed[16];
    char buf[64], w[24];
    bool ok = r->scope == AKIRA_SCOPE_ONCE || r->scope == AKIRA_SCOPE_ALWAYS;

    akira_display_clear(bg());
    akira_seal_sigil_seed(e, seed);
    if (!mono()) {
        akira_logo_glow(76, 118, 70, ok ? dom_colour(d) : faint(), 70);
    }
    crystal_draw_sigil(76, 118, 52, seed, ok ? akira_logo_mix(dom_colour(d), ink(), 90) : dim());

    int x = 152, y = 16;

    snprintf(buf, sizeof(buf), "Seal #%u", r->counter);
    akira_display_text_large(x, y, buf, ink());
    y += 26;
    akira_display_text(x, y, akira_seal_op_title(r->op), ok ? dom_colour(d) : dim());
    y += 20;

    snprintf(buf, sizeof(buf), "App     %s", r->app);
    akira_display_text(x, y, buf, ink());
    y += 15;
    snprintf(buf, sizeof(buf), "Scope   %s", scope_name(r->scope));
    akira_display_text(x, y, buf, ink());
    y += 15;
    when(r->epoch, r->uptime_s, w, sizeof(w));
    snprintf(buf, sizeof(buf), "When    %s", w);
    akira_display_text(x, y, buf, ink());
    y += 15;
    if (r->detail[0]) {
        /* up to two lines of the reason, under the fields */
        int cols = (SCR_W - x - 6) / 8;
        int n = (int)strlen(r->detail);

        for (int off = 0, line = 0; off < n && line < 2; off += cols, line++) {
            snprintf(buf, sizeof(buf), "%.*s", cols, r->detail + off);
            akira_display_text(x, y, buf, dim());
            y += 13;
        }
        y += 2;
    }
    y += 6;
    akira_display_text(x, y, e->signed_by_se ? "Signed by SE050" : "Unsigned",
                       e->signed_by_se ? dom_colour(AKIRA_DOM_NET) : warn());
    y += 15;
    snprintf(buf, sizeof(buf), "Chain %02x%02x%02x%02x%02x%02x", r->prev[0], r->prev[1],
             r->prev[2], r->prev[3], r->prev[4], r->prev[5]);
    akira_display_text(x, y, buf, faint());
    y += 15;
    snprintf(buf, sizeof(buf), "Build %08x", r->app_id);
    akira_display_text(x, y, buf, faint());

    text_right(SCR_W - 10, 226, "B back", faint());
}

static void detail_key(uint32_t just)
{
    if (just & (BIT(AKIRA_BTN_B) | BIT(AKIRA_BTN_A))) {
        screen_pop();
    }
}

static const shell_screen_t detail_scr = {
    .name = "akira_seal_detail",
    .flags = 0,
    .enter = detail_enter,
    .on_key = detail_key,
    .draw = detail_draw,
};

/* ======================================================================== */
/* How it works — four pages, one of them a practice Seal                    */
/* ======================================================================== */

#define INTRO_PAGES 4
#define PRACTICE_LAP1 1000
#define PRACTICE_LAP2 1300
#define RING_X (SCR_W / 2)
#define RING_Y 132
#define RING_R 38
#define RING_BOX (RING_R + 8)

static struct {
    int page;
    bool first_run;
    int64_t page_t0;
    int64_t hold_t0;   /* practice: A+B held since; 0 = not held */
    int result;        /* practice: -1 none, 0 short, 1 this time, 2 always, 3 said no */
    bool b_alone;      /* practice: B went down without A */
    akira_crystal_view_t v;
} g_intro AKIRA_BULK_BSS;

static void keycap(int x, int y, const char *label, uint16_t c)
{
    akira_display_rounded_rect(x, y, 15, 15, 4, c);
    akira_display_text(x + 4, y + 3, label, c);
}

static void centred_text(int y, const char *s, uint16_t c)
{
    akira_display_text((SCR_W - akira_ui_text_w(s, AKIRA_UI_FONT_SMALL)) / 2, y, s, c);
}

static void centred_large(int y, const char *s, uint16_t c)
{
    akira_display_text_large((SCR_W - akira_ui_text_w(s, AKIRA_UI_FONT_LARGE)) / 2, y, s, c);
}

static void lines(int x, int y, const char *const *l, int n, uint16_t c)
{
    for (int i = 0; i < n; i++) {
        akira_display_text(x, y + i * 13, l[i], c);
    }
}

static int practice_charge(void)
{
    return g_intro.hold_t0 ? (int)(k_uptime_get() - g_intro.hold_t0) : 0;
}

static void intro_draw(void)
{
    const int64_t t = k_uptime_get() - g_intro.page_t0;

    akira_display_clear(bg());

    switch (g_intro.page) {
    case 0: {
        crystal_fx_t fx;

        if (!mono()) {
            akira_logo_glow(84, 110, 72, 0x29D3u, 160);
        }
        crystal_fx_init(&fx, 84, 110, 56, bg());
        fx.grow_ms = (int)MIN(t, (int64_t)CRYSTAL_GROWN - 1);
        crystal_draw(&g_intro.v, &fx);
        akira_display_text_large(160, 30, "Meet Akira", ink());
        static const char *const l[] = {
            "Your console's", "crystal. It grows", "as you use the", "console.", "",
            "Nothing to set up.", "Nothing to feed.",
        };
        lines(160, 62, l, ARRAY_SIZE(l), dim());
        break;
    }
    case 1: {
        centred_large(12, "You stay in charge", ink());
        centred_text(38, "Some apps want powerful things:", dim());
        centred_text(51, "radio, Wi-Fi attacks, typing as a", dim());
        centred_text(64, "keyboard. AkiraOS asks you first.", dim());

        /* the practice ring around a small Akira */
        crystal_fx_t fx;
        int charge = practice_charge();
        const int top = -CRYSTAL_TURN / 4;
        uint16_t acc = mono() ? 0x0000u : akira_domain_colour(AKIRA_DOM_RADIO);

        crystal_fx_init(&fx, RING_X, RING_Y, 26, bg());
        fx.focus = AKIRA_DOM_RADIO;
        fx.focus_glow = MIN(charge, PRACTICE_LAP1) * 256 / PRACTICE_LAP1;
        crystal_draw(&g_intro.v, &fx);
        crystal_draw_arc(RING_X, RING_Y, RING_R, 2, top, top + CRYSTAL_TURN,
                         mono() ? 0x0000u : akira_logo_mix(bg(), acc, 60));
        if (charge > 0) {
            crystal_draw_arc(RING_X, RING_Y, RING_R, 4, top,
                             top + MIN(charge, PRACTICE_LAP1) * CRYSTAL_TURN / PRACTICE_LAP1,
                             mono() ? 0x0000u : akira_logo_mix(acc, 0xFFFFu, 90));
        }
        if (charge > PRACTICE_LAP1) {
            crystal_draw_arc(RING_X, RING_Y, RING_R + 6, 2, top,
                             top + MIN(charge - PRACTICE_LAP1, PRACTICE_LAP2) * CRYSTAL_TURN /
                                       PRACTICE_LAP2,
                             ink());
        }

        const char *msg;

        if (charge >= PRACTICE_LAP1 + PRACTICE_LAP2) {
            msg = "Two rings: always. Let go!";
        } else if (charge >= PRACTICE_LAP1) {
            msg = "Allowed once - hold for always";
        } else if (charge > 0) {
            msg = "Keep holding...";
        } else if (g_intro.result == 2) {
            msg = "That's a Seal - always allowed.";
        } else if (g_intro.result == 1) {
            msg = "That's a Seal - allowed once.";
        } else if (g_intro.result == 0) {
            msg = "Let go too soon - nothing allowed.";
        } else if (g_intro.result == 3) {
            msg = "B on its own says no.";
        } else {
            msg = NULL;
        }
        if (msg) {
            centred_text(184, msg, g_intro.result >= 1 && g_intro.result <= 2 && !charge
                                       ? acc : ink());
        } else {
            /* "Try it: [A] + [B]" */
            int x = (SCR_W - 152) / 2;

            akira_display_text(x, 186, "Try it: hold", ink());
            keycap(x + 102, 183, "A", ink());
            akira_display_text(x + 121, 186, "+", ink());
            keycap(x + 133, 183, "B", ink());
        }
        centred_text(204, "1 ring = this time   2 rings = always", faint());
        break;
    }
    case 2: {
        static const char *const what[AKIRA_DOM_COUNT] = {
            "transmitting on radio", "Bluetooth & Wi-Fi tricks", "internet & app installs",
            "USB, keyboard, pins",   "secure-element keys",      "game records",
        };
        char n[8];

        centred_large(12, "What grows Akira", ink());
        centred_text(36, "Each power you allow adds a shard:", dim());
        for (int d = 0; d < AKIRA_DOM_COUNT; d++) {
            int y = 54 + d * 17;

            diamond(22, y + 5, 5, dom_colour(d));
            akira_display_text(34, y, akira_domain_name(d), ink());
            akira_display_text(84, y, what[d], dim());
            snprintf(n, sizeof(n), "%u", g_intro.v.shards[d]);
            text_right(SCR_W - 14, y, n, g_intro.v.shards[d] ? ink() : faint());
        }
        centred_text(164, "The core grows with the days you use it.", dim());
        centred_text(177, "Glints come from game achievements.", dim());
        break;
    }
    default: {
        /* a sample crystal with a crack, so the word has a picture */
        akira_crystal_view_t demo = g_intro.v;
        crystal_fx_t fx;

        if (demo.shards[AKIRA_DOM_RADIO] == 0) {
            demo.shards[AKIRA_DOM_RADIO] = 1;
            demo.uses[AKIRA_DOM_RADIO] = 4;
        }
        demo.cracks[AKIRA_DOM_RADIO] = 1;
        crystal_fx_init(&fx, 84, 110, 54, bg());
        crystal_draw(&demo, &fx);
        akira_display_text_large(160, 22, "Cracks", ink());
        static const char *const l[] = {
            "If an app tries a", "power it never", "asked for, AkiraOS", "blocks it, and",
            "Akira cracks.",     "",               "Settings > Akira",  "& Seals > Cracks",
            "shows who. Mend",   "it, or remove",  "the app.",
        };
        lines(160, 48, l, ARRAY_SIZE(l), dim());
        break;
    }
    }

    /* page dots and keys */
    for (int i = 0; i < INTRO_PAGES; i++) {
        int x = SCR_W / 2 - (INTRO_PAGES - 1) * 7 + i * 14;

        if (i == g_intro.page) {
            akira_display_rounded_rect_fill(x - 4, 220, 8, 5, 2, ink());
        } else {
            akira_display_circle_fill(x, 222, 1, faint());
        }
    }
    if (g_intro.page > 0) {
        akira_display_text(10, 226, "< back", faint());
    } else {
        akira_display_text(10, 226, g_intro.first_run ? "B skip" : "B close", faint());
    }
    text_right(SCR_W - 10, 226,
               g_intro.page == INTRO_PAGES - 1 ? "A done"
               : g_intro.page == 1             ? "next >"
                                               : "A next >",
               faint());
}

static void intro_finish(void)
{
#ifdef CONFIG_AKIRA_SETTINGS
    akira_settings_set("akira/intro", "1", 0);
#endif
    screen_pop();
}

static void intro_go(int page)
{
    g_intro.page = CLAMP(page, 0, INTRO_PAGES - 1);
    g_intro.page_t0 = k_uptime_get();
    g_intro.hold_t0 = 0;
    g_intro.result = -1;
    screen_invalidate();
}

static void intro_key(uint32_t just)
{
    /* On the practice page A and B belong to the ring. */
    bool practice = (g_intro.page == 1);

    if (just & BIT(AKIRA_BTN_RIGHT) || (!practice && (just & BIT(AKIRA_BTN_A)))) {
        if (g_intro.page == INTRO_PAGES - 1) {
            intro_finish();
        } else {
            intro_go(g_intro.page + 1);
        }
        return;
    }
    if ((just & BIT(AKIRA_BTN_LEFT)) && g_intro.page > 0) {
        intro_go(g_intro.page - 1);
        return;
    }
    if (!practice && (just & BIT(AKIRA_BTN_B))) {
        intro_finish();
    }
}

static bool intro_tick(void)
{
    if (g_intro.page == 0) {
        if (k_uptime_get() - g_intro.page_t0 < crystal_grow_duration(&g_intro.v) + 100) {
            screen_invalidate_rect(84 - 70, 110 - 70, 140, 140);
        }
        return false;
    }
    if (g_intro.page != 1) {
        return false;
    }
    const uint32_t bits = akira_input_get_bitmask();
    const bool a = bits & BIT(AKIRA_BTN_A), b = bits & BIT(AKIRA_BTN_B);

    if (a && b) {
        g_intro.b_alone = false;
        if (!g_intro.hold_t0) {
            g_intro.hold_t0 = k_uptime_get();
            g_intro.result = -1;
            return true;
        }
        int c = practice_charge();

        /* the message changes at each lap: a whole frame then */
        static int s_lap;
        int lap = c >= PRACTICE_LAP1 + PRACTICE_LAP2 ? 2 : c >= PRACTICE_LAP1 ? 1 : 0;

        if (lap != s_lap) {
            s_lap = lap;
            return true;
        }
        screen_invalidate_rect(RING_X - RING_BOX, RING_Y - RING_BOX, RING_BOX * 2,
                               RING_BOX * 2);
        return false;
    }
    if (g_intro.hold_t0) {
        int c = practice_charge();

        g_intro.result = c >= PRACTICE_LAP1 + PRACTICE_LAP2 ? 2 : c >= PRACTICE_LAP1 ? 1 : 0;
        g_intro.hold_t0 = 0;
        return true;
    }
    if (b && !a) {
        g_intro.b_alone = true;
    } else if (!b && g_intro.b_alone) {
        g_intro.b_alone = false;
        g_intro.result = 3;
        return true;
    }
    return false;
}

static void intro_enter(void *arg)
{
    memset(&g_intro, 0, sizeof(g_intro));
    g_intro.first_run = (arg != NULL);
    akira_crystal_get(&g_intro.v);
    intro_go(0);
}

static const shell_screen_t intro_scr = {
    .name = "akira_intro",
    .flags = 0,
    .enter = intro_enter,
    .on_key = intro_key,
    .tick = intro_tick,
    .draw = intro_draw,
};

void akira_intro_open(bool first_run)
{
    akira_crystal_init();
    screen_push(&intro_scr, first_run ? (void *)1 : NULL);
}

void akira_intro_first_run_check(void)
{
    static bool s_done;

    if (s_done || k_uptime_get() < 6000) {
        return;
    }
    s_done = true;
#ifdef CONFIG_AKIRA_SETTINGS
    char v[4] = "";

    if (akira_settings_get("akira/intro", v, sizeof(v)) == 0 && v[0] == '1') {
        return;
    }
#endif
    akira_intro_open(true);
}
