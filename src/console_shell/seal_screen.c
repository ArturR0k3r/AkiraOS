/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_seal_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_seal_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file seal_screen.c
 * @brief The Seal ritual (see seal_screen.h).
 *
 * Layout (SCR_W x 240, dark):
 *   y=12   domain chip ("◆ RADIO")                         "SEAL"
 *   y=30   what is being sealed, large ("Transmit on radio")
 *   y=54   who and why ("rf_raw · transmit captured RF?")
 *   centre Akira, the requested domain lit, the rest dimmed; around it the
 *          ring that charges while A+B are held.  If this seal will grow a
 *          new shard, a translucent "ghost" shard fills in with the charge.
 *   y=204  state line ("Hold A+B to seal" → "Keep holding: always" → ...)
 *   y=224  hints / the refusal countdown
 *
 * While the ring charges only its box is presented (screen_invalidate_rect);
 * phase changes and the stamp present whole frames.
 */

#include "seal_screen.h"
#include "shell_theme.h"
#include "ui/akira_logo.h"
#include "ui/akira_ui.h"
#include "ui/anim.h"
#include "ui/crystal_render.h"
#include "ui/screen.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <drivers/platform_hal.h>
#include <lib/mem_helper.h>

/* ---- timing (ms) ------------------------------------------------------ */
#define LAP1_MS   1000 /* one lap: THIS TIME */
#define LAP2_MS   1300 /* the second lap: ALWAYS */
#define STAMP_MS  1700
#define REFUSE_MS 750
#define TAP_MS    600 /* a B press shorter than this, with no A, refuses */

/* ---- layout ------------------------------------------------------------ */
#define CX     (SCR_W / 2)
#define CY     136
#define CRYS_R 38
#define RING_R 54
#define RING_W 4
#define BOX    (RING_R + RING_W + 6) /* half-size of the ring's dirty box */

typedef enum { P_WAIT, P_CHARGE, P_DRAIN, P_STAMP, P_REFUSE } phase_t;

static struct {
    akira_seal_req_t *req;
    void (*on_closed)(akira_seal_req_t *req);
    bool open;
    bool answered;
    bool closing;
    phase_t phase;
    int64_t opened;
    int64_t phase_t0;
    int64_t hold_t0; /* when the current A+B hold (virtually) began */
    int drain_from;  /* ms of charge when the hold broke */
    bool armed;      /* A and B were both released since opening */
    int64_t b_down_at;
    bool a_seen;
    bool new_pair;   /* sealing grows a new shard */
    bool lap1_done;
    int last_secs_left;
    uint8_t sigil[16];
    akira_crystal_view_t view;
    char sub[64];
} g AKIRA_BULK_BSS;

static int64_t now_ms(void)
{
    return k_uptime_get();
}

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

static uint16_t dim_ink(void)
{
    return mono() ? 0x0000u : 0x8C97u; /* cool lavender gray */
}

static akira_domain_t domain(void)
{
    return akira_seal_op_domain(g.req->op);
}

static uint16_t accent(void)
{
    return mono() ? 0x0000u : akira_domain_colour(domain());
}

/* Charge in ms of hold (0 .. LAP1+LAP2). */
static int charge_ms(void)
{
    switch (g.phase) {
    case P_CHARGE:
        return (int)(now_ms() - g.hold_t0);
    case P_DRAIN:
        return MAX(g.drain_from - (int)(now_ms() - g.phase_t0) * 3, 0);
    default:
        return 0;
    }
}

static void set_phase(phase_t p)
{
    g.phase = p;
    g.phase_t0 = now_ms();
    screen_invalidate();
}

static void answer(akira_seal_scope_t scope)
{
    if (g.answered) {
        return;
    }
    g.answered = true;
    akira_seal_finish(g.req, scope); /* records, signs, grows Akira */
    akira_seal_sigil_seed(&g.req->entry, g.sigil);
    akira_crystal_get(&g.view);
    set_phase((g.req->scope == AKIRA_SCOPE_ONCE || g.req->scope == AKIRA_SCOPE_ALWAYS)
                  ? P_STAMP : P_REFUSE);
}

/* ---- drawing -------------------------------------------------------------- */

static void centred(int y, const char *s, int font, uint16_t c)
{
    int x = MAX((SCR_W - akira_ui_text_w(s, font)) / 2, 4);

    if (font == AKIRA_UI_FONT_LARGE) {
        akira_display_text_large(x, y, s, c);
    } else {
        akira_display_text(x, y, s, c);
    }
}

/* What saying yes actually lets the app do, in a sentence. */
static const char *risk_line(akira_seal_op_t op)
{
    switch (op) {
    case AKIRA_SEAL_RF_TX:       return "It can interfere with radios nearby.";
    case AKIRA_SEAL_WIFI_INJECT: return "It can kick devices off Wi-Fi.";
    case AKIRA_SEAL_BLE_SPAM:    return "It can flood nearby phones with pop-ups.";
    case AKIRA_SEAL_HID_INJECT:  return "It can type and click on the computer.";
    case AKIRA_SEAL_VAULT_SIGN:  return "It can sign in as you with a stored key.";
    case AKIRA_SEAL_APP_INSTALL: return "A new app will be added to the console.";
    case AKIRA_SEAL_USB_SHARE:   return "The computer can read and change the SD card.";
    case AKIRA_SEAL_RECORD:      return "Your record is signed so it can be trusted.";
    default:                     return "";
    }
}

static void keycap(int x, int y, const char *label, uint16_t c)
{
    akira_display_rounded_rect(x, y, 15, 15, 4, c);
    akira_display_text(x + 4, y + 3, label, c);
}

static void draw_header(void)
{
    char chip[16];
    const char *title = g.req->op == AKIRA_SEAL_RECORD ? "Seal this record?"
                                                        : akira_seal_op_title(g.req->op);

    snprintf(chip, sizeof(chip), "%s", akira_domain_name(domain()));
    for (char *p = chip; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p -= 32;
        }
    }
    akira_display_triangle_fill(16, 15, 20, 11, 24, 15, accent());
    akira_display_triangle_fill(16, 15, 20, 19, 24, 15, accent());
    akira_display_text(30, 10, chip, accent());
    akira_display_text(SCR_W - 16 - akira_ui_text_w("SEAL", AKIRA_UI_FONT_SMALL), 10, "SEAL",
                       dim_ink());

    centred(28, title, AKIRA_UI_FONT_LARGE, ink());
    centred(50, g.sub, AKIRA_UI_FONT_SMALL, ink());
    centred(64, risk_line(g.req->op), AKIRA_UI_FONT_SMALL, dim_ink());
}

static void draw_ring(int charge)
{
    const int top = -CRYSTAL_TURN / 4; /* 12 o'clock */
    const uint16_t track = mono() ? 0x0000u : akira_logo_mix(bg(), accent(), 60);

    if (mono()) {
        /* dotted track */
        for (int i = 0; i < 48; i += 2) {
            crystal_draw_arc(CX, CY, RING_R, 1, top + i * CRYSTAL_TURN / 48,
                             top + (i + 1) * CRYSTAL_TURN / 48, track);
        }
    } else {
        crystal_draw_arc(CX, CY, RING_R, 2, top, top + CRYSTAL_TURN, track);
    }
    if (charge <= 0) {
        return;
    }
    int lap1 = MIN(charge, LAP1_MS);
    uint16_t c1 = mono() ? 0x0000u : akira_logo_mix(accent(), 0xFFFFu, 90);

    crystal_draw_arc(CX, CY, RING_R, RING_W, top, top + lap1 * CRYSTAL_TURN / LAP1_MS, c1);
    if (charge > LAP1_MS && g.req->allow_always) {
        int lap2 = MIN(charge - LAP1_MS, LAP2_MS);

        crystal_draw_arc(CX, CY, RING_R + RING_W + 1, 2, top,
                         top + lap2 * CRYSTAL_TURN / LAP2_MS, ink());
    }
}

static void draw_crystal(int charge, int64_t t)
{
    crystal_fx_t fx;
    int glow = MIN(charge, LAP1_MS) * 256 / LAP1_MS;

    crystal_fx_init(&fx, CX, CY, CRYS_R, bg());
    fx.focus = domain();
    fx.dim = 130 - glow * 50 / 256;
    fx.focus_glow = MAX(glow, 70); /* the asked-for domain glows from the start */
    fx.ghost = g.new_pair && !g.answered ? MAX(glow, 30) : 0;
    /* a slow sway so it's alive while it waits */
    fx.angle = (int)((t / 8) % CRYSTAL_TURN) / 16;

    if (g.phase == P_STAMP) {
        int st = (int)(now_ms() - g.phase_t0);

        fx.flash = MAX(256 - st * 256 / 450, 0);
        fx.regrow_ms = g.req->new_shard ? MAX(st - 650, 0) : -1;
        fx.dim = MAX(90 - st * 90 / 600, 0);
        fx.focus_glow = MAX(256 - st * 256 / 1200, 0);
    } else if (g.phase == P_REFUSE) {
        fx.dim = 200;
        fx.focus_glow = 0;
        fx.ghost = 0;
    }
    if (!mono()) {
        akira_logo_glow(CX, CY, RING_R + 4, accent(),
                        g.phase == P_REFUSE ? 0 : 40 + glow * 120 / 256);
    }
    crystal_draw(&g.view, &fx);
}

static void draw_status(int charge)
{
    char line[48];
    const int y = 202;

    switch (g.phase) {
    case P_STAMP:
        snprintf(line, sizeof(line), "%s",
                 g.req->scope == AKIRA_SCOPE_ALWAYS ? "Allowed - always" : "Allowed - this time");
        centred(y, line, AKIRA_UI_FONT_SMALL, accent());
        centred(y + 16, g.req->entry.signed_by_se ? "Signed and written to the history"
                                                  : "Written to the history",
                AKIRA_UI_FONT_SMALL, dim_ink());
        return;
    case P_REFUSE:
        centred(y, g.req->scope == AKIRA_SCOPE_TIMEOUT ? "No answer - not allowed" : "Not allowed",
                AKIRA_UI_FONT_SMALL, ink());
        centred(y + 16, "The app keeps running without it", AKIRA_UI_FONT_SMALL, dim_ink());
        return;
    default:
        break;
    }

    if (charge >= LAP1_MS && g.req->allow_always) {
        centred(y, "Allowed once - keep holding for always", AKIRA_UI_FONT_SMALL, ink());
    } else if (charge > 0) {
        centred(y, "Keep holding...", AKIRA_UI_FONT_SMALL, ink());
    } else {
        /* [A] + [B]  hold to allow */
        int x = (SCR_W - 176) / 2;

        keycap(x, y - 3, "A", ink());
        akira_display_text(x + 19, y, "+", ink());
        keycap(x + 31, y - 3, "B", ink());
        akira_display_text(x + 54, y, "hold to allow", ink());
    }
    if (g.last_secs_left > 0 && g.last_secs_left <= 10) {
        char cd[32];

        snprintf(cd, sizeof(cd), "No answer in %ds means no", g.last_secs_left);
        centred(y + 18, cd, AKIRA_UI_FONT_SMALL, dim_ink());
    } else {
        centred(y + 18,
                g.req->allow_always ? "2 rings = always   B alone = no"
                                    : "B alone = no",
                AKIRA_UI_FONT_SMALL, dim_ink());
    }
}

static void draw_stamp(void)
{
    int st = (int)(now_ms() - g.phase_t0);
    uint16_t c = mono() ? 0x0000u : akira_logo_mix(accent(), 0xFFFFu, 120);
    int r, x, y;

    if (st < 900) {
        /* slam: big and fast, settling with a little bounce */
        r = 66 - 32 * anim_ease(ANIM_EASE_OUT_BACK, st, 360) / 256;
        x = CX;
        y = CY;
    } else {
        /* then it tucks into the corner, like wax on a letter */
        int p = anim_ease(ANIM_EASE_INOUT, st - 900, 380);

        r = 34 - 18 * p / 256;
        x = CX + (RING_R - 10) * p / 256;
        y = CY + (RING_R - 18) * p / 256;
    }
    if (!mono() && st < 900) {
        /* a dark disc under the slam so the rune reads over the crystal */
        akira_logo_burst(x, y, r + 2, 6, bg());
    }
    crystal_draw_sigil(x, y, r, g.sigil, c);
}

static void seal_draw(void)
{
    int64_t t = now_ms() - g.opened;
    int charge = charge_ms();

    akira_display_clear(bg());
    draw_header();
    draw_crystal(charge, t);
    if (g.phase != P_STAMP && g.phase != P_REFUSE) {
        draw_ring(charge);
    } else if (g.phase == P_STAMP) {
        /* the ring bursts outward as it seals */
        int st = (int)(now_ms() - g.phase_t0);

        if (st < 420) {
            int grow = anim_ease(ANIM_EASE_OUT, st, 420);
            uint16_t c = akira_logo_mix(akira_logo_mix(accent(), 0xFFFFu, 160), bg(), grow);

            crystal_draw_arc(CX, CY, RING_R + grow * 30 / 256, RING_W + 2 - grow * 4 / 256, 0,
                             CRYSTAL_TURN, c);
        }
        draw_stamp();
    }
    draw_status(charge);
}

/* ---- behaviour ------------------------------------------------------------ */

static void close_now(void)
{
    if (!g.open || g.closing) {
        return;
    }
    g.closing = true;
    screen_pop(); /* -> seal_leave() */
}

static bool seal_tick(void)
{
    const int64_t now = now_ms();
    const uint32_t bits = akira_input_get_bitmask();
    const bool a = bits & BIT(AKIRA_BTN_A), b = bits & BIT(AKIRA_BTN_B);
    bool full = false;

    if (g.req->should_cancel && !g.answered && g.req->should_cancel()) {
        answer(AKIRA_SCOPE_REFUSED);
    }

    switch (g.phase) {
    case P_WAIT:
    case P_DRAIN:
        if (!a && !b) {
            g.armed = true;
        }
        if (g.armed && a && b) {
            /* resume from whatever charge is left in the ring */
            g.hold_t0 = now - charge_ms();
            g.lap1_done = false;
            set_phase(P_CHARGE);
            break;
        }
        /* a quick B on its own refuses */
        if (g.armed) {
            if (b && !g.b_down_at) {
                g.b_down_at = now;
                g.a_seen = a;
            } else if (b && a) {
                g.a_seen = true;
            } else if (!b && g.b_down_at) {
                bool tap = !g.a_seen && (now - g.b_down_at) < TAP_MS;

                g.b_down_at = 0;
                if (tap) {
                    answer(AKIRA_SCOPE_REFUSED);
                    break;
                }
            }
        }
        if (g.phase == P_DRAIN && charge_ms() == 0) {
            set_phase(P_WAIT);
        }
        break;

    case P_CHARGE: {
        int held = (int)(now - g.hold_t0);

        if (held >= LAP1_MS && !g.lap1_done) {
            g.lap1_done = true;
            if (!g.req->allow_always) {
                answer(AKIRA_SCOPE_ONCE);
                break;
            }
            full = true; /* the state line changes */
        }
        if (!(a && b)) {
            if (held >= LAP1_MS) {
                answer(AKIRA_SCOPE_ONCE); /* let go during the second lap */
            } else {
                g.drain_from = held;
                set_phase(P_DRAIN);
            }
            break;
        }
        if (held >= LAP1_MS + LAP2_MS) {
            answer(AKIRA_SCOPE_ALWAYS);
        }
        break;
    }

    case P_STAMP:
        if (now - g.phase_t0 >= STAMP_MS) {
            close_now();
            return false;
        }
        return true;

    case P_REFUSE:
        if (now - g.phase_t0 >= REFUSE_MS) {
            close_now();
            return false;
        }
        return true;
    }

    /* Unanswered for too long: refuse. */
    if (!g.answered && g.phase != P_CHARGE) {
        int left = CONFIG_AKIRA_SEAL_TIMEOUT_S - (int)((now - g.opened) / 1000);

        if (left <= 0) {
            answer(AKIRA_SCOPE_TIMEOUT);
            return true;
        }
        if (left != g.last_secs_left) {
            g.last_secs_left = left;
            if (left <= 10) {
                full = true;
            }
        }
    }
    if (g.answered || full) {
        return true;
    }
    /* Ring, ghost shard and sway all live inside the ring's box. */
    screen_invalidate_rect(CX - BOX, CY - BOX, BOX * 2, BOX * 2);
    return false;
}

static void seal_enter(void *arg)
{
    ARG_UNUSED(arg);
}

static void seal_leave(void)
{
    akira_seal_req_t *req = g.req;
    void (*closed)(akira_seal_req_t *) = g.on_closed;

    if (!g.answered) {
        g.answered = true;
        akira_seal_finish(req, AKIRA_SCOPE_REFUSED);
    }
    g.open = false;
    if (closed) {
        closed(req); /* the shell gives the display back first ... */
    }
    akira_seal_release(req); /* ... then the asker wakes */
}

static void seal_key(uint32_t just)
{
    ARG_UNUSED(just); /* the hold is read from the button levels in tick() */
}

static const shell_screen_t seal_scr = {
    .name = "seal",
    .flags = 0, /* full-bleed */
    .enter = seal_enter,
    .leave = seal_leave,
    .on_key = seal_key,
    .tick = seal_tick,
    .draw = seal_draw,
};

void seal_screen_open(akira_seal_req_t *req, void (*on_closed)(akira_seal_req_t *req))
{
    memset(&g, 0, sizeof(g));
    g.req = req;
    g.on_closed = on_closed;
    g.open = true;
    g.opened = now_ms();
    g.phase = P_WAIT;
    g.phase_t0 = g.opened;
    g.last_secs_left = CONFIG_AKIRA_SEAL_TIMEOUT_S;
    akira_crystal_get(&g.view);
    g.new_pair = !akira_crystal_pair_known(domain(), req->app, req->op);
    if (req->detail[0]) {
        snprintf(g.sub, sizeof(g.sub), "%s  -  %s", req->app, req->detail);
    } else {
        snprintf(g.sub, sizeof(g.sub), "%s wants this power", req->app);
    }
    /* keep it to one line */
    int max_chars = (SCR_W - 24) / 8;

    if ((int)strlen(g.sub) > max_chars) {
        g.sub[max_chars - 2] = '.';
        g.sub[max_chars - 1] = '.';
        g.sub[max_chars] = '\0';
    }
    LOG_INF("Seal asked: %s by %s", akira_seal_op_title(req->op), req->app);
    screen_push(&seal_scr, NULL);
}

bool seal_screen_is_open(void)
{
    return g.open;
}

void seal_screen_cancel(void)
{
    if (!g.open) {
        return;
    }
    if (!g.answered) {
        g.answered = true;
        akira_seal_finish(g.req, AKIRA_SCOPE_REFUSED);
    }
    if (screen_is_open(&seal_scr)) {
        g.closing = true;
        screen_close(&seal_scr); /* -> seal_leave() */
    }
}
