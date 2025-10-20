/**
 * @file akira_sim.c
 * @brief Akira Console SDL2 Visual Simulator - Main Implementation
 *
 * Creates and manages the visual simulation window for the Akira Console
 */

/* Include Zephyr headers first to avoid type conflicts */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Then include system/SDL2 headers */
#include <pthread.h>
#include <SDL2/SDL.h>

#include "akira_sim.h"

LOG_MODULE_REGISTER(akira_sim, LOG_LEVEL_INF);

/* SDL components */
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static bool sim_running = false;
static pthread_t sim_thread;
static pthread_mutex_t sim_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Framebuffer reference */
static const uint16_t *current_framebuffer = NULL;
static bool framebuffer_updated = false;

/* External functions from submodules */
extern int akira_sim_display_init(SDL_Renderer *renderer);
extern void akira_sim_display_cleanup(void);
extern void akira_sim_display_update(SDL_Renderer *renderer, const uint16_t *framebuffer);
extern void akira_sim_display_render(SDL_Renderer *renderer);

extern int akira_sim_buttons_init(void);
extern void akira_sim_buttons_handle_mouse(int x, int y, bool pressed);
extern void akira_sim_buttons_handle_keyboard(SDL_Keycode key, bool pressed);
extern uint32_t akira_sim_buttons_get_state(void);
extern void akira_sim_buttons_render(SDL_Renderer *renderer);

/**
 * @brief SDL event processing and rendering loop
 */
static void *simulator_thread(void *arg)
{
    SDL_Event event;
    bool mouse_down = false;

    LOG_INF("Simulator thread started");

    while (sim_running)
    {
        /* Process SDL events */
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_QUIT:
                LOG_INF("Window close requested");
                sim_running = false;
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT)
                {
                    mouse_down = true;
                    akira_sim_buttons_handle_mouse(
                        event.button.x, event.button.y, true);
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT)
                {
                    mouse_down = false;
                    akira_sim_buttons_handle_mouse(
                        event.button.x, event.button.y, false);
                }
                break;

            case SDL_MOUSEMOTION:
                if (mouse_down)
                {
                    akira_sim_buttons_handle_mouse(
                        event.motion.x, event.motion.y, true);
                }
                break;

            case SDL_KEYDOWN:
                akira_sim_buttons_handle_keyboard(event.key.keysym.sym, true);
                break;

            case SDL_KEYUP:
                akira_sim_buttons_handle_keyboard(event.key.keysym.sym, false);
                break;
            }
        }

        /* Render frame */
        pthread_mutex_lock(&sim_mutex);

        /* Clear background to gray */
        SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
        SDL_RenderClear(renderer);

        /* Update display if framebuffer changed */
        if (framebuffer_updated && current_framebuffer)
        {
            akira_sim_display_update(renderer, current_framebuffer);
            framebuffer_updated = false;
        }

        /* Render display and buttons */
        akira_sim_display_render(renderer);
        akira_sim_buttons_render(renderer);

        /* Draw window title area */
        SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
        SDL_Rect title_bar = {0, 0, SIM_WINDOW_WIDTH, 40};
        SDL_RenderFillRect(renderer, &title_bar);

        /* Present frame */
        SDL_RenderPresent(renderer);

        pthread_mutex_unlock(&sim_mutex);

        /* Run at ~60 FPS */
        SDL_Delay(16);
    }

    LOG_INF("Simulator thread exiting");
    return NULL;
}

int akira_sim_init(void)
{
    LOG_INF("Initializing Akira Console Simulator");

    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0)
    {
        LOG_ERR("SDL initialization failed: %s", SDL_GetError());
        return -1;
    }

    /* Create window */
    window = SDL_CreateWindow(
        "Akira Console Simulator",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SIM_WINDOW_WIDTH,
        SIM_WINDOW_HEIGHT,
        SDL_WINDOW_SHOWN);

    if (!window)
    {
        LOG_ERR("Window creation failed: %s", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    /* Create renderer */
    renderer = SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!renderer)
    {
        LOG_ERR("Renderer creation failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    /* Initialize display simulation */
    if (akira_sim_display_init(renderer) < 0)
    {
        LOG_ERR("Display simulation init failed");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    /* Initialize button simulation */
    if (akira_sim_buttons_init() < 0)
    {
        LOG_ERR("Button simulation init failed");
        akira_sim_display_cleanup();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    /* Start simulator thread */
    sim_running = true;
    if (pthread_create(&sim_thread, NULL, simulator_thread, NULL) != 0)
    {
        LOG_ERR("Failed to create simulator thread");
        sim_running = false;
        akira_sim_display_cleanup();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    LOG_INF("✅ Akira Console Simulator initialized successfully");
    LOG_INF("Window size: %dx%d", SIM_WINDOW_WIDTH, SIM_WINDOW_HEIGHT);

    return 0;
}

void akira_sim_shutdown(void)
{
    if (!sim_running)
    {
        return;
    }

    LOG_INF("Shutting down simulator");

    sim_running = false;
    pthread_join(sim_thread, NULL);

    akira_sim_display_cleanup();

    if (renderer)
    {
        SDL_DestroyRenderer(renderer);
        renderer = NULL;
    }

    if (window)
    {
        SDL_DestroyWindow(window);
        window = NULL;
    }

    SDL_Quit();

    LOG_INF("Simulator shutdown complete");
}

void akira_sim_update_display(const uint16_t *framebuffer)
{
    if (!framebuffer)
    {
        return;
    }

    pthread_mutex_lock(&sim_mutex);
    current_framebuffer = framebuffer;
    framebuffer_updated = true;
    pthread_mutex_unlock(&sim_mutex);
}

uint32_t akira_sim_get_button_state(void)
{
    return akira_sim_buttons_get_state();
}

bool akira_sim_is_running(void)
{
    return sim_running;
}

void akira_sim_render(void)
{
    /* Rendering happens in the simulator thread */
}

uint32_t akira_sim_process_events(void)
{
    /* Event processing happens in the simulator thread */
    return akira_sim_get_button_state();
}
