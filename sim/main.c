/*
 * AkiraConsole Simulator — main entry point
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Usage:
 *   akira_sim <app.wasm> [--sdcard <dir>]
 *
 * Keyboard controls:
 *   W/A/S/D or Arrow keys  D-pad
 *   Z / Enter              A button (confirm)
 *   X / Escape             B button (back)
 *   Q                      X button (HOME — long-press simulation via hold)
 *   C                      Y button (context)
 *   F                      Toggle display 1× / 2× scale
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include <SDL2/SDL.h>
#include <wasm_export.h>

#include "host/sim_display.h"
#include "host/sim_input.h"
#include "host/sim_storage.h"
#include "wamr/wamr_host.h"

/* -----------------------------------------------------------------------
 * Window layout
 * --------------------------------------------------------------------- */

#define SCALE_DEFAULT   2          /* 320×240 → 640×480 logical display  */
#define BEZEL_TOP       50         /* pixels above display                */
#define BEZEL_BOTTOM    140        /* pixels below display (buttons area) */
#define BEZEL_SIDES     40         /* pixels left/right of display        */

static int g_scale = SCALE_DEFAULT;

/* Computed from scale */
static int win_w(void) { return DISP_W * g_scale + BEZEL_SIDES * 2; }
static int win_h(void) { return DISP_H * g_scale + BEZEL_TOP + BEZEL_BOTTOM; }

/* Display rect in window coords */
static SDL_Rect disp_rect(void)
{
    return (SDL_Rect){
        .x = BEZEL_SIDES,
        .y = BEZEL_TOP,
        .w = DISP_W * g_scale,
        .h = DISP_H * g_scale,
    };
}

/* -----------------------------------------------------------------------
 * Button descriptor (for bezel drawing)
 * --------------------------------------------------------------------- */

typedef struct {
    const char *label;
    int         code;
    int         wx, wy; /* window-relative position */
    int         r;      /* radius */
} bezel_btn_t;

static bezel_btn_t g_btns[] = {
    /* D-pad */
    { "U", SIM_BTN_UP,    0,  -40, 20 },
    { "D", SIM_BTN_DOWN,  0,   40, 20 },
    { "L", SIM_BTN_LEFT, -40,   0, 20 },
    { "R", SIM_BTN_RIGHT, 40,   0, 20 },
    /* Action */
    { "A", SIM_BTN_A,     0,    0, 20 },
    { "B", SIM_BTN_B,   -40,    0, 20 },
    { "X", SIM_BTN_X,     0,  -40, 20 },
    { "Y", SIM_BTN_Y,    40,    0, 20 },
};

/* -----------------------------------------------------------------------
 * WAMR state
 * --------------------------------------------------------------------- */

#define WASM_STACK_SIZE  (256 * 1024)
#define WASM_HEAP_SIZE   (2  * 1024 * 1024)
#define WASM_BUF_MAX     (8  * 1024 * 1024)

static wasm_module_t       g_module;
static wasm_module_inst_t  g_inst;
static wasm_exec_env_t     g_exec_env;

/* WASM execution runs on its own thread */
static pthread_t g_wasm_thread;
static bool      g_wasm_running;

typedef struct { const char *name; } wasm_thread_arg_t;

static void *wasm_thread_fn(void *arg)
{
    (void)arg;

    /* wasm_runtime_init_thread_env installs SIGSEGV/SIGBUS signal handlers
     * for this OS thread so WAMR can catch WASM stack overflows gracefully.
     * Required on any thread that calls wasm_runtime_call_wasm. */
    if (!wasm_runtime_init_thread_env()) {
        fprintf(stderr, "[sim] Failed to init WAMR thread env\n");
        g_wasm_running = false;
        return NULL;
    }

    wasm_function_inst_t fn = wasm_runtime_lookup_function(g_inst, "main");
    if (!fn) fn = wasm_runtime_lookup_function(g_inst, "_start");
    if (!fn) {
        fprintf(stderr, "[sim] WASM: no 'main' or '_start' export found\n");
    } else {
        wasm_exec_env_t env = wasm_runtime_get_exec_env_singleton(g_inst);
        if (!env) {
            fprintf(stderr, "[sim] No singleton exec_env\n");
        } else {
            /* WASI-compiled main is always exported as main(int argc, char **argv)
             * even when the C source is main(void).  wasm_runtime_prepare_call_function
             * (active with WASM_ENABLE_REF_TYPES=1) sets param_argc=param_cell_num
             * but keeps argv=NULL when argc=0, causing word_copy(lp,NULL,2) → abort.
             * Fix: always supply a 2-element zeroed arg buffer; for main(void) apps
             * wasm_runtime_prepare_call_function will use param_cell_num=0 and
             * ignore the extra words. */
            uint32_t wasm_argv[2] = {0, 0}; /* argc=0 (i32), argv=0 (i32 ptr) */
            if (!wasm_runtime_call_wasm(env, fn, 2, wasm_argv)) {
                const char *ex = wasm_runtime_get_exception(g_inst);
                if (ex && ex[0])
                    fprintf(stderr, "[sim] WASM exception: %s\n", ex);
            }
        }
    }

    wasm_runtime_destroy_thread_env();

    g_wasm_running = false;
    return NULL;
}

/* -----------------------------------------------------------------------
 * SDL bezel rendering helpers
 * --------------------------------------------------------------------- */

static void draw_circle_filled(SDL_Renderer *r, int cx, int cy, int rad,
                                SDL_Color col)
{
    SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);
    for (int dy = -rad; dy <= rad; dy++)
        for (int dx = -rad; dx <= rad; dx++)
            if (dx*dx + dy*dy <= rad*rad)
                SDL_RenderDrawPoint(r, cx+dx, cy+dy);
}

static void render_bezel(SDL_Renderer *r, uint32_t btn_mask)
{
    int W = win_w(), H = win_h();

    /* Body */
    SDL_SetRenderDrawColor(r, 28, 28, 32, 255);
    SDL_RenderClear(r);

    /* Display frame */
    SDL_SetRenderDrawColor(r, 50, 50, 58, 255);
    SDL_Rect frame = { BEZEL_SIDES - 6, BEZEL_TOP - 6,
                       DISP_W*g_scale + 12, DISP_H*g_scale + 12 };
    SDL_RenderFillRect(r, &frame);

    /* Title text area */
    SDL_SetRenderDrawColor(r, 40, 40, 48, 255);
    SDL_Rect title = { 0, 0, W, BEZEL_TOP - 8 };
    SDL_RenderFillRect(r, &title);

    /* D-pad cluster (left side of button area) */
    int dpad_cx = W / 4;
    int btn_cy  = BEZEL_TOP + DISP_H * g_scale + BEZEL_BOTTOM / 2;

    static const int dpad_offsets[][2] = {{0,-38},{0,38},{-38,0},{38,0}};
    static const int dpad_codes[]      = {SIM_BTN_UP, SIM_BTN_DOWN,
                                          SIM_BTN_LEFT, SIM_BTN_RIGHT};
    static const char *dpad_labels[]   = {"W", "S", "A", "D"};
    for (int i = 0; i < 4; i++) {
        bool held = !!(btn_mask & (1u << dpad_codes[i]));
        SDL_Color col = held ? (SDL_Color){220,220,80,255}
                             : (SDL_Color){80,80,90,255};
        draw_circle_filled(r,
            dpad_cx + dpad_offsets[i][0],
            btn_cy  + dpad_offsets[i][1], 18, col);
        /* outline */
        SDL_SetRenderDrawColor(r, 120,120,130,255);
    }

    /* Action buttons (right side) */
    int act_cx  = W * 3 / 4;
    static const int act_offsets[][2] = {{0,-38},{-38,0},{0,38},{38,0}};
    static const int act_codes[]      = {SIM_BTN_X, SIM_BTN_B,
                                         SIM_BTN_A, SIM_BTN_Y};
    static const char *act_labels[]   = {"Q(X)", "X(B)", "Z(A)", "C(Y)"};
    for (int i = 0; i < 4; i++) {
        bool held = !!(btn_mask & (1u << act_codes[i]));
        SDL_Color col = held ? (SDL_Color){80,220,80,255}
                             : (SDL_Color){80,80,90,255};
        draw_circle_filled(r,
            act_cx + act_offsets[i][0],
            btn_cy + act_offsets[i][1], 18, col);
    }

    /* Keyboard hints — simple ASCII, SDL has no font, we draw small rects */
    /* (A real UI would use SDL_ttf; for now the rects show which is active) */
    (void)g_btns; /* suppress unused warning */
    (void)dpad_labels; (void)act_labels;
}

/* -----------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    const char *wasm_path  = NULL;
    const char *sdcard_dir = "./sdcard";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sdcard") == 0 && i+1 < argc) {
            sdcard_dir = argv[++i];
        } else if (argv[i][0] != '-') {
            wasm_path = argv[i];
        }
    }

    if (!wasm_path) {
        fprintf(stderr, "Usage: akira_sim <app.wasm> [--sdcard <dir>]\n");
        return 1;
    }

    /* ---- WAMR init -------------------------------------------------- */
    char errbuf[256];
    RuntimeInitArgs init = {0};
    init.mem_alloc_type           = Alloc_With_Pool;
    init.mem_alloc_option.pool.heap_buf  = malloc(WASM_HEAP_SIZE);
    init.mem_alloc_option.pool.heap_size = WASM_HEAP_SIZE;

    if (!wasm_runtime_full_init(&init)) {
        fprintf(stderr, "[sim] WAMR init failed\n");
        return 1;
    }

    if (!wamr_host_register_natives()) {
        fprintf(stderr, "[sim] Failed to register native symbols\n");
        return 1;
    }

    /* ---- Load WASM -------------------------------------------------- */
    FILE *fp = fopen(wasm_path, "rb");
    if (!fp) { perror(wasm_path); return 1; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);
    if (fsize <= 0 || fsize > WASM_BUF_MAX) {
        fprintf(stderr, "[sim] WASM file size invalid: %ld\n", fsize);
        fclose(fp); return 1;
    }
    uint8_t *wasm_buf = malloc((size_t)fsize);
    fread(wasm_buf, 1, (size_t)fsize, fp);
    fclose(fp);

    g_module = wasm_runtime_load(wasm_buf, (uint32_t)fsize, errbuf, sizeof(errbuf));
    if (!g_module) {
        fprintf(stderr, "[sim] Load failed: %s\n", errbuf);
        return 1;
    }

    /* heap_size=8192: WAMR needs a small managed heap for internal bookkeeping
     * (e.g. exec_env frame init).  The old value of WASM_HEAP_SIZE (2 MB) caused
     * WAMR to try carving 2 MB from the module's 64 KB linear memory, corrupting
     * frame pointers and triggering the word_copy(src=NULL) assertion.
     * 8 KB is well within the 64 KB module limit and satisfies WAMR's needs. */
    g_inst = wasm_runtime_instantiate(g_module, WASM_STACK_SIZE,
                                      8192, errbuf, sizeof(errbuf));
    if (!g_inst) {
        fprintf(stderr, "[sim] Instantiate failed: %s\n", errbuf);
        return 1;
    }

    /* Use the singleton exec_env — creating a second exec_env with
     * wasm_runtime_create_exec_env corrupts the instance's function table. */
    g_exec_env = wasm_runtime_get_exec_env_singleton(g_inst);
    if (!g_exec_env) {
        fprintf(stderr, "[sim] Failed to get singleton exec env\n");
        return 1;
    }

    /* ---- Host subsystems -------------------------------------------- */
    sim_input_init();
    sim_storage_set_root(sdcard_dir);

    /* ---- SDL2 ------------------------------------------------------- */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "[sim] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow(
        "AkiraConsole Simulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w(), win_h(),
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "[sim] SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "[sim] SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }

    if (sim_display_init(ren) < 0) {
        fprintf(stderr, "[sim] Display init failed\n");
        return 1;
    }

    printf("[sim] AkiraConsole Simulator\n");
    printf("[sim] Loading: %s\n", wasm_path);
    printf("[sim] SD card: %s\n", sdcard_dir);
    printf("[sim] Controls: WASD/Arrows=D-pad  Z=A  X=B  Q=HOME  C=Y\n");
    printf("[sim] Press F to toggle scale (%d×)\n", g_scale);

    /* ---- Start WASM thread ------------------------------------------ */
    g_wasm_running = true;
    pthread_create(&g_wasm_thread, NULL, wasm_thread_fn, NULL);

    /* ---- Main render / event loop ----------------------------------- */
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_f) {
                    /* Toggle scale 1× / 2× */
                    g_scale = (g_scale == 1) ? 2 : 1;
                    SDL_SetWindowSize(win, win_w(), win_h());
                } else {
                    sim_input_handle_key(ev.key.keysym.sym, true);
                }
                break;
            case SDL_KEYUP:
                sim_input_handle_key(ev.key.keysym.sym, false);
                break;
            }
        }

        /* WASM app done? */
        if (!g_wasm_running) running = false;

        uint32_t btn_mask = sim_input_get_bitmask();

        render_bezel(ren, btn_mask);

        SDL_Rect dr = disp_rect();
        sim_display_render(ren, &dr);

        SDL_RenderPresent(ren);
        SDL_Delay(16); /* ~60 fps */
    }

    /* ---- Cleanup ---------------------------------------------------- */
    pthread_join(g_wasm_thread, NULL);

    sim_display_cleanup();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

    /* g_exec_env is the singleton — freed automatically by deinstantiate */
    wasm_runtime_deinstantiate(g_inst);
    wasm_runtime_unload(g_module);
    wasm_runtime_destroy();

    free(wasm_buf);
    free(init.mem_alloc_option.pool.heap_buf);

    return 0;
}
