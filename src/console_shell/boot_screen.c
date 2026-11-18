/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_boot_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_boot_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file boot_screen.c
 * @brief Animated AkiraOS boot splash (see boot_screen.h).
 *
 * Timeline (ms from the first frame the panel can actually show):
 *
 *     0 ─ 1210  GROW   the crystal grows along its spikes from the core, the
 *                      growth front white-hot; the logo settles from 90 %
 *   250 ─ 2100  SPARK  five twinkles pop near the spike tips, staggered
 *  1000 ─ 1710  WORD   "AkiraOS" rises in letter by letter
 *  1050 ─ 1700  SHINE  a diagonal glint sweeps the finished crystal
 *  1650 ─ 2050  VER    version line fades in
 *     … ─ 2700  HOLD   the glow behind the crystal breathes
 *  2700 ─ 2880  FLARE  the crystal flares to pure light, the halo blazes
 *  2880 ─ 3300  BURST  a disc of the shell's background bursts out of the
 *                      crystal and fills the screen; the home screen's
 *                      cards then fan out (home_screen.c)
 *
 * Every value is a pure function of the animation clock, so a slow frame
 * never slows the animation — it only skips a step.  The clock itself pauses
 * through a stall (the shell thread starved while main() brings the system
 * up), within a budget, so a busy boot doesn't eat the opening.
 *
 * SPI budget: a full ST7789V frame is ~120 ms at 10 MHz, so only BURST
 * (which really does change every pixel) presents whole frames.  Everything
 * before it changes inside the glow disc and the text block and is presented
 * as that rect (screen_invalidate_rect): ~35-45 ms a frame, about 25 fps.
 */

#include "boot_screen.h"
#include "home_screen.h"
#include "shell_theme.h"
#include "ui/akira_logo.h"
#include "ui/akira_ui.h"
#include "ui/anim.h"
#include "ui/screen.h"
#if defined(CONFIG_AKIRA_CRYSTAL)
#include "ui/crystal_render.h"
#include <crystal/akira_crystal.h>
#endif

#include <zephyr/kernel.h>
#include <string.h>

#include <app_version.h>
#include <api/akira_display_api.h>
#include <drivers/display/fonts.h>
#include <drivers/platform_hal.h>
#include <lib/mem_helper.h>

/* ---- timeline (ms) ------------------------------------------------------ */
#define T_GROW     0
#define D_GROW     1210
#define T_SHINE    1050
#define D_SHINE    650
#define T_WORD     1000
#define S_LETTER   55   /* stagger between letters */
#define D_LETTER   380
#define D_INK      240  /* a letter's fade from background to ink */
#define T_VER      1650
#define D_VER      400
#define T_EXIT     2700
#define D_FLARE    180
#define D_BURST    420
#define D_EXIT     (D_FLARE + D_BURST)
#define D_SPARK    520
#define BREATH_MS  1700
#define STALL_MS   150  /* a frame gap this long means we were starved... */
#define STALL_MAX  2000 /* ...and the clock may pause this much in total */

/* ---- layout (SCR_W x 240) ----------------------------------------------- */
#define LOGO_CY    86
#define GLOW_R     72
#define WORD_Y     160
#define WORD_RISE  14   /* px a letter rises through */
#define WORD_GAP   2    /* px between letters' ink (proportional layout) */
#define VER_Y      198

static const char k_word[] = "AkiraOS";
static const char k_ver[] = "v" APP_VERSION_STRING;

/* Twinkles near the spike tips: offset from the crystal's centre, start. */
static const struct {
    int8_t dx, dy;
    uint16_t t0;
    uint8_t size;
} k_sparks[] = {
    { 44, -42, 250, 7 }, { -52, -16, 520, 6 }, { 50, 24, 760, 7 },
    { -30, 48, 1020, 5 }, { 12, -58, 1580, 6 },
};

/* ---- palette ------------------------------------------------------------ */
/* Colour panel: the crystal on deep navy.  1-bit panel: ink on paper — the
 * MONO conversion would turn every non-black colour white. */
typedef struct {
    uint16_t bg, ink, dim, glow, spark;
} boot_palette_t;

static const boot_palette_t k_colour = {
    .bg = AKIRA_BOOT_SPLASH_BG,
    .ink = 0xFFFFu,
    .dim = 0x8C97u,   /* #8A93B8 — cool lavender gray */
    .glow = 0x29D3u,  /* #2E3A9E — indigo halo */
    .spark = 0xCFFFu, /* #C8FCFF — icy white */
};
static const boot_palette_t k_mono = {
    .bg = 0xFFFFu, .ink = 0x0000u, .dim = 0x0000u, .glow = 0xFFFFu, .spark = 0x0000u,
};

/* ---- state -------------------------------------------------------------- */
static struct {
    bool started;   /* the panel is up and t0 is set */
    int64_t t0;     /* animation clock origin (pushed later by stalls) */
    int64_t last;   /* previous tick, for stall detection */
    int stalled;    /* ms the clock has paused so far */
    int64_t ready_wait; /* when we started waiting for Akira's state */
#if defined(CONFIG_AKIRA_CRYSTAL)
    akira_crystal_view_t view; /* this console's Akira, grown on boot */
    int grow_dur;
#endif
    const boot_palette_t *pal;
} g_boot AKIRA_BULK_BSS; /* internal DRAM is the scarce resource */

static int elapsed(void)
{
    return (int)(k_uptime_get() - g_boot.t0);
}

/* Eased 0..256 progress of the window [t0, t0 + dur) at time t. */
static int prog(int t, int t0, int dur, anim_ease_t ease)
{
    return anim_ease(ease, t - t0, dur);
}

/* Triangle wave 0..256..0 over @p period. */
static int breathe(int t)
{
    int ph = t % BREATH_MS;
    int half = BREATH_MS / 2;

    return ph < half ? ph * 256 / half : (BREATH_MS - ph) * 256 / half;
}

/* Ink columns [l, r] of a hero-font glyph — the font is fixed-width, but a
 * wordmark set on fixed cells reads "Aki raOS". */
static void glyph_ink(char c, int *l, int *r)
{
    uint16_t m = 0;

    if (c >= FONT16X28_FIRST_CHAR && c <= FONT16X28_LAST_CHAR) {
        for (int row = 0; row < FONT16X28_HEIGHT; row++) {
            m |= font_hero[c - FONT16X28_FIRST_CHAR][row];
        }
    }
    if (!m) {
        *l = 0;
        *r = FONT16X28_WIDTH / 2 - 1;
        return;
    }
    *l = __builtin_clz(m) - 16;     /* bit 15 = leftmost column */
    *r = 15 - __builtin_ctz(m);
}

static int word_w(void)
{
    int w = 0, l, r;

    for (size_t i = 0; i < sizeof(k_word) - 1; i++) {
        glyph_ink(k_word[i], &l, &r);
        w += (r - l + 1) + (i ? WORD_GAP : 0);
    }
    return w;
}

static int word_x(void)
{
    return (SCR_W - word_w()) / 2;
}

static int ver_x(void)
{
    return (SCR_W - akira_ui_text_w(k_ver, AKIRA_UI_FONT_SMALL)) / 2;
}

static bool panel_ready(void)
{
    int w = 0, h = 0;

    akira_display_get_size(&w, &h);
    return w > 0 && h > 0;
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_scene(int t)
{
    const boot_palette_t *p = g_boot.pal;
    const int cx = SCR_W / 2;
    const int flare = prog(t, T_EXIT, D_FLARE, ANIM_EASE_OUT);
    const int burst = prog(t, T_EXIT + D_FLARE, D_BURST, ANIM_EASE_INOUT);

    akira_display_clear(p->bg);

    /* Halo: swells in with the growth, flares under the glint, breathes
     * through the hold, then blazes with the crystal. */
    int glow = prog(t, T_GROW, 900, ANIM_EASE_INOUT) * 190 / 256;
    int lift = prog(t, T_SHINE, D_SHINE / 2, ANIM_EASE_OUT) -
               prog(t, T_SHINE + D_SHINE / 2, D_SHINE / 2, ANIM_EASE_INOUT);

    glow += lift * 66 / 256;
    if (t > T_SHINE + D_SHINE && t < T_EXIT) {
        glow += (breathe(t - T_SHINE - D_SHINE) - 128) * 36 / 256;
    }
    glow += (256 - glow) * flare / 256;
    akira_logo_glow(cx, LOGO_CY, GLOW_R, akira_logo_mix(p->glow, p->spark, flare * 3 / 4),
                    glow);

    /* The crystal: grows, settles 90 % → 100 %, flares white. */
    int grown = prog(t, T_GROW, D_GROW, ANIM_EASE_OUT);
    int16_t tips[16];
    int ntips = 0;

#if defined(CONFIG_AKIRA_CRYSTAL)
    /* This console's own Akira, growing from its core. */
    crystal_fx_t cfx;
    const int R = 60;

    crystal_fx_init(&cfx, cx, LOGO_CY, R * 90 / 100 + R * 10 / 100 * grown / 256, p->bg);
    cfx.grow_ms = t >= T_GROW + D_GROW ? CRYSTAL_GROWN
                                        : MAX(t - T_GROW, 0) * g_boot.grow_dur / D_GROW;
    cfx.angle = (256 - grown) * 2400 / 256; /* unwinds ~13° as it grows */
    cfx.flash = flare;
    if (t >= T_SHINE && t < T_SHINE + D_SHINE) {
        cfx.shine = -20 + prog(t, T_SHINE, D_SHINE, ANIM_EASE_INOUT) * (4 * R + 40) / 256;
    }
    crystal_draw(&g_boot.view, &cfx);
    ntips = crystal_tips(tips, ARRAY_SIZE(tips) / 2);
#else
    akira_logo_fx_t fx = {
        .reveal = grown * AKIRA_LOGO_REVEAL_END / 256,
        .shine = AKIRA_LOGO_NO_SHINE,
        .opacity = 256,
        .scale = 230 + grown * 26 / 256,
        .flash = flare,
    };

    if (t >= T_SHINE && t < T_SHINE + D_SHINE) {
        int span = akira_logo_w(AKIRA_LOGO_LARGE) + akira_logo_h(AKIRA_LOGO_LARGE) + 40;

        fx.shine = -20 + prog(t, T_SHINE, D_SHINE, ANIM_EASE_INOUT) * span / 256;
    }
    akira_logo_draw(AKIRA_LOGO_LARGE, cx, LOGO_CY, &fx);
#endif

    /* Twinkles: pop up fast, fade slower — at real shard tips when we have
     * them, else at fixed spots around the logo. */
    for (size_t i = 0; i < ARRAY_SIZE(k_sparks); i++) {
        int st = t - k_sparks[i].t0;

        if (st < 0 || st >= D_SPARK) {
            continue;
        }
        int up = anim_ease(ANIM_EASE_OUT, st, D_SPARK * 35 / 100);
        int down = anim_ease(ANIM_EASE_INOUT, st - D_SPARK * 35 / 100, D_SPARK * 65 / 100);
        int size = k_sparks[i].size * (up - down) / 256;
        int sx = cx + k_sparks[i].dx, sy = LOGO_CY + k_sparks[i].dy;

        if (ntips > 0) {
            int k = (int)(i * 3 + 1) % ntips;

            sx = tips[k * 2];
            sy = tips[k * 2 + 1];
        }
        akira_logo_sparkle(sx, sy, size, p->spark);
    }

    /* Wordmark, letter by letter: rise with a little overshoot, ink in. */
    int x = word_x();

    for (size_t i = 0; i < sizeof(k_word) - 1; i++) {
        int lt = t - T_WORD - (int)i * S_LETTER;
        int l, r;

        glyph_ink(k_word[i], &l, &r);
        if (lt > 0) {
            char ch[2] = { k_word[i], '\0' };
            int rise = WORD_RISE * (256 - anim_ease(ANIM_EASE_OUT_BACK, lt, D_LETTER)) / 256;
            int ink = anim_ease(ANIM_EASE_OUT, lt, D_INK);

            akira_display_text_huge(x - l, WORD_Y + rise, ch, akira_logo_mix(p->bg, p->ink, ink));
        }
        x += (r - l + 1) + WORD_GAP;
    }

    /* Version line — bows out as the crystal flares. */
    if (t >= T_VER) {
        int ink = prog(t, T_VER, D_VER, ANIM_EASE_INOUT) * (256 - flare) / 256;

        akira_display_text(ver_x(), VER_Y, k_ver, akira_logo_mix(p->bg, p->dim, ink));
    }

    /* Burst: the shell's background erupts from the crystal, soft-edged,
     * until it covers the far corner. */
    if (burst > 0) {
        const int far = 226; /* centre → farthest corner, plus the soft edge */

        akira_logo_burst(cx, LOGO_CY, burst * far / 256, 12, C_BLACK);
    }
}

/* What changed since the last frame, as one rect for a partial present. */
static void invalidate_changes(int t)
{
    const int cx = SCR_W / 2;
    const int word_end = T_WORD + (int)(sizeof(k_word) - 2) * S_LETTER + D_LETTER;

    /* Glow disc (it breathes until EXIT): contains the crystal and twinkles. */
    screen_invalidate_rect(cx - GLOW_R - 1, LOGO_CY - GLOW_R - 1, 2 * GLOW_R + 3,
                           2 * GLOW_R + 3);
    if (t >= T_WORD && t < word_end + 40) {
        int w = word_w();

        /* Letters move through WORD_RISE px and overshoot a couple above. */
        screen_invalidate_rect(word_x() - 2, WORD_Y - 4, w + 4, 28 + WORD_RISE + 8);
    }
    if ((t >= T_VER && t < T_VER + D_VER + 40) || t >= T_EXIT) {
        screen_invalidate_rect(ver_x() - 2, VER_Y - 1, akira_ui_text_w(k_ver, 0) + 4, 12);
    }
}

/* ---- screen callbacks --------------------------------------------------- */

static void boot_enter(void *arg)
{
    ARG_UNUSED(arg);
    memset(&g_boot, 0, sizeof(g_boot));
    g_boot.pal = &k_colour;
}

static void boot_leave(void)
{
    /* However the splash ended — on its own, a skip, or HOME — the launcher
     * arrives with its fan-out. */
    home_screen_play_intro();
}

static void boot_key(uint32_t just)
{
    if (just && g_boot.started && elapsed() < T_EXIT) {
        g_boot.t0 = k_uptime_get() - T_EXIT; /* skip straight to the flare */
    }
}

static bool boot_tick(void)
{
    if (!g_boot.started) {
        /* The shell thread can get here before main() has brought the
         * display HAL up; start the clock at the first frame that shows. */
        if (!panel_ready()) {
            return false;
        }
#if defined(CONFIG_AKIRA_CRYSTAL)
        /* Grow the real Akira: give storage a moment to be readable. */
        if (!g_boot.ready_wait) {
            g_boot.ready_wait = k_uptime_get();
        }
        if (akira_crystal_init() != 0 && k_uptime_get() - g_boot.ready_wait < 1500) {
            return false;
        }
        akira_crystal_get(&g_boot.view);
        g_boot.grow_dur = crystal_grow_duration(&g_boot.view);
#endif
        g_boot.started = true;
        g_boot.t0 = k_uptime_get();
        g_boot.last = g_boot.t0;
        g_boot.pal = akira_logo_panel_is_mono() ? &k_mono : &k_colour;
        LOG_INF("Boot splash");
        return true;
    }

    int64_t now = k_uptime_get();
    int gap = (int)(now - g_boot.last);

    g_boot.last = now;
    if (gap > STALL_MS && g_boot.stalled < STALL_MAX && elapsed() < T_EXIT) {
        /* We didn't get to run: hold the clock rather than skip ahead. */
        int hold = MIN(gap - 33, STALL_MAX - g_boot.stalled);

        g_boot.t0 += hold;
        g_boot.stalled += hold;
    }

    int t = elapsed();

    if (t >= T_EXIT + D_EXIT) {
        screen_pop(); /* -> boot_leave() -> home fan-out */
        return true;
    }
    if (t >= T_EXIT + D_FLARE) {
        return true; /* the burst changes every pixel: whole frames */
    }
    invalidate_changes(t);
    return false;
}

static void boot_draw(void)
{
    if (!g_boot.started) {
        akira_display_clear(AKIRA_BOOT_SPLASH_BG);
        return;
    }
    draw_scene(MIN(elapsed(), T_EXIT + D_EXIT));
}

const shell_screen_t boot_screen = {
    .name = "boot",
    .flags = 0, /* full-bleed: no status bar, no footer */
    .enter = boot_enter,
    .leave = boot_leave,
    .on_key = boot_key,
    .tick = boot_tick,
    .draw = boot_draw,
};
