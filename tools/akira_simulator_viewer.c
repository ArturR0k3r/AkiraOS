/**
 * @file akira_simulator_viewer.c
 * @brief Standalone SDL2 Viewer for Akira Console
 *
 * This external process displays the Akira Console simulator window
 * by reading framebuffer data from shared memory.
 *
 * Build:
 *   gcc -o akira_viewer akira_simulator_viewer.c `pkg-config --cflags --libs sdl2` -lrt -lm
 *
 * Run:
 *   ./akira_viewer &
 *   (then run zephyr.exe in another terminal)
 */

#include <SDL2/SDL.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <SDL2/SDL_image.h>

/* Display dimensions */
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 320

/* Window dimensions */
#define WINDOW_WIDTH 400
#define WINDOW_HEIGHT 600

/* Display position in window */
#define DISPLAY_X 80
#define DISPLAY_Y 60

/* Shared memory names */
#define SHM_FRAMEBUFFER "/akira_framebuffer"
#define SHM_BUTTONS "/akira_buttons"

/* Button definitions */
#define NUM_BUTTONS 10

typedef struct
{
    int x;
    int y;
    int radius;
    const char *label;
} button_t;

/* Button layout matching hardware */
static button_t buttons[NUM_BUTTONS] = {
    {350, 100, 20, "PWR"}, // Power
    {50, 100, 20, "SET"},  // Settings
    {80, 450, 25, "↑"},    // Up
    {80, 520, 25, "↓"},    // Down
    {45, 485, 25, "←"},    // Left
    {115, 485, 25, "→"},   // Right
    {320, 450, 25, "X"},   // X
    {320, 520, 25, "B"},   // B
    {285, 485, 25, "Y"},   // Y
    {355, 485, 25, "A"}    // A
};

/* Global state */
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *display_texture = NULL;
static SDL_Texture *pcb_texture = NULL;
static uint16_t *framebuffer_shm = NULL;
static uint32_t *buttons_shm = NULL;
static uint32_t button_state = 0;
static bool running = true;

/* Convert RGB565 to RGB888 */
static uint32_t rgb565_to_rgb888(uint16_t color)
{
    uint8_t r = ((color >> 11) & 0x1F) << 3;
    uint8_t g = ((color >> 5) & 0x3F) << 2;
    uint8_t b = (color & 0x1F) << 3;

    r |= (r >> 5);
    g |= (g >> 6);
    b |= (b >> 5);

    return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

/* Return true if framebuffer appears blank (all zeros or all same color) */
static bool is_framebuffer_blank()
{
    if (!framebuffer_shm)
        return true;

    uint16_t first = framebuffer_shm[0];
    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        if (framebuffer_shm[i] != 0 && framebuffer_shm[i] != first)
            return false;
    }
    return true;
}

/* Render a simple AKIRA splash into the provided pixels buffer (RGBA8888) */
static void render_akira_splash(uint32_t *pixels)
{
    /* background gradient */
    for (int y = 0; y < DISPLAY_HEIGHT; y++)
    {
        uint8_t g = 30 + (y * 120) / DISPLAY_HEIGHT; /* subtle gradient */
        uint32_t rowcolor = (0xFF << 24) | (20 << 16) | (g << 8) | 40;
        for (int x = 0; x < DISPLAY_WIDTH; x++)
        {
            pixels[y * DISPLAY_WIDTH + x] = rowcolor;
        }
    }

    /* Draw stylized AKIRA using blocks (no font dependency) */
    int cx = DISPLAY_WIDTH / 2;
    int cy = DISPLAY_HEIGHT / 2;
    int w = 120;
    int h = 40;
    int spacing = 8;
    uint32_t fg = (0xFF << 24) | (240 << 16) | (240 << 8) | 240;

    /* Draw a rounded rectangle behind text */
    SDL_Rect bg = {cx - w / 2 - 8, cy - h / 2 - 8, w + 16, h + 16};
    for (int yy = bg.y; yy < bg.y + bg.h; yy++)
    {
        if (yy < 0 || yy >= DISPLAY_HEIGHT)
            continue;
        for (int xx = bg.x; xx < bg.x + bg.w; xx++)
        {
            if (xx < 0 || xx >= DISPLAY_WIDTH)
                continue;
            pixels[yy * DISPLAY_WIDTH + xx] = (0xFF << 24) | (40 << 16) | (40 << 8) | 40;
        }
    }

    /* Very simple block letters for AKIRA */
    const char *letters = "AKIRA";
    int letter_w = 18;
    int letter_h = 34;
    int start_x = cx - ((int)strlen(letters) * (letter_w + spacing)) / 2;

    for (int L = 0; L < (int)strlen(letters); L++)
    {
        int lx = start_x + L * (letter_w + spacing);
        int ly = cy - letter_h / 2;
        /* fill block */
        for (int yy = ly; yy < ly + letter_h; yy++)
        {
            if (yy < 0 || yy >= DISPLAY_HEIGHT)
                continue;
            for (int xx = lx; xx < lx + letter_w; xx++)
            {
                if (xx < 0 || xx >= DISPLAY_WIDTH)
                    continue;
                /* carve some holes to make letters vary a bit */
                if (((xx + yy) % 7) < 6)
                    pixels[yy * DISPLAY_WIDTH + xx] = fg;
            }
        }
    }
}

/* Check if point is in circle */
static bool point_in_circle(int px, int py, int cx, int cy, int radius)
{
    int dx = px - cx;
    int dy = py - cy;
    return (dx * dx + dy * dy) <= (radius * radius);
}

/* Handle mouse button */
static void handle_mouse(int x, int y, bool pressed)
{
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        if (point_in_circle(x, y, buttons[i].x, buttons[i].y, buttons[i].radius))
        {
            if (pressed)
            {
                button_state |= (1 << i);
            }
            else
            {
                button_state &= ~(1 << i);
            }
            if (buttons_shm)
            {
                *buttons_shm = button_state;
            }
            printf("Button %d (%s) %s\n", i, buttons[i].label,
                   pressed ? "pressed" : "released");
            break;
        }
    }
}

/* Handle keyboard */
static void handle_keyboard(SDL_Keycode key, bool pressed)
{
    int button_id = -1;

    switch (key)
    {
    case SDLK_ESCAPE:
        button_id = 0;
        break; // Power
    case SDLK_RETURN:
        button_id = 1;
        break; // Settings
    case SDLK_w:
        button_id = 2;
        break; // Up
    case SDLK_s:
        button_id = 3;
        break; // Down
    case SDLK_a:
        button_id = 4;
        break; // Left
    case SDLK_d:
        button_id = 5;
        break; // Right
    case SDLK_i:
        button_id = 6;
        break; // X
    case SDLK_k:
        button_id = 7;
        break; // B
    case SDLK_j:
        button_id = 8;
        break; // Y
    case SDLK_l:
        button_id = 9;
        break; // A
    }

    if (button_id >= 0)
    {
        if (pressed)
        {
            button_state |= (1 << button_id);
        }
        else
        {
            button_state &= ~(1 << button_id);
        }
        if (buttons_shm)
        {
            *buttons_shm = button_state;
        }
        printf("Button %d (%s) %s\n", button_id, buttons[button_id].label,
               pressed ? "pressed" : "released");
    }
}

/* Render display from framebuffer */
static void render_display()
{
    if (!framebuffer_shm || !display_texture)
    {
        return;
    }

    /* Convert RGB565 to RGB888 */
    uint32_t pixels[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    bool blank = is_framebuffer_blank();
    if (blank)
    {
        /* Render fallback splash */
        render_akira_splash(pixels);
    }
    else
    {
        for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
        {
            pixels[i] = rgb565_to_rgb888(framebuffer_shm[i]);
        }
    }

    /* Update texture */
    SDL_UpdateTexture(display_texture, NULL, pixels, DISPLAY_WIDTH * sizeof(uint32_t));

    /* Draw display in correct LCD region */
    SDL_Rect display_rect = {DISPLAY_X, DISPLAY_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT};
    SDL_RenderCopy(renderer, display_texture, NULL, &display_rect);
}

/* Render buttons */
static void render_buttons()
{
    /* Draw PCB photo as background */
    if (pcb_texture)
    {
        SDL_Rect bg_rect = {0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
        SDL_RenderCopy(renderer, pcb_texture, NULL, &bg_rect);
    }

    /* Draw buttons */
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        bool pressed = (button_state & (1 << i)) != 0;
        button_t *btn = &buttons[i];

        /* Fill circle */
        if (pressed)
        {
            SDL_SetRenderDrawColor(renderer, 255, 150, 20, 255);
        }
        else
        {
            SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
        }

        for (int dy = -btn->radius; dy <= btn->radius; dy++)
        {
            for (int dx = -btn->radius; dx <= btn->radius; dx++)
            {
                if (dx * dx + dy * dy <= btn->radius * btn->radius)
                {
                    SDL_RenderDrawPoint(renderer, btn->x + dx, btn->y + dy);
                }
            }
        }
    }

    /* Draw "AKIRA" label */
    /* small logo on PCB */
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_Rect logo = {18, 428, 56, 20};
    SDL_RenderFillRect(renderer, &logo);
    SDL_SetRenderDrawColor(renderer, 18, 100, 48, 255);
    SDL_Rect logo_cut = {22, 432, 48, 12};
    SDL_RenderFillRect(renderer, &logo_cut);
}

/* Main loop */
int main(int argc, char *argv[])
{
    printf("🎮 Akira Console Simulator Viewer\n");
    printf("==================================\n\n");


    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0)
    {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    if (IMG_Init(IMG_INIT_PNG) == 0)
    {
        fprintf(stderr, "SDL_image PNG init failed: %s\n", IMG_GetError());
        SDL_Quit();
        return 1;
    }

    /* Create window */
    window = SDL_CreateWindow(
        "Akira Console Simulator",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SDL_WINDOW_SHOWN);

    if (!window)
    {
        fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }


    /* Create renderer */
    renderer = SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!renderer)
    {
        fprintf(stderr, "Renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }


    /* Load PCB photo as background */
    SDL_Surface *pcb_surface = IMG_Load("tools/akira_pcb.png");
    if (!pcb_surface)
    {
        fprintf(stderr, "Failed to load PCB PNG: %s\n", IMG_GetError());
    }
    else
    {
        pcb_texture = SDL_CreateTextureFromSurface(renderer, pcb_surface);
        SDL_FreeSurface(pcb_surface);
    }

    /* Create display texture */
    display_texture = SDL_CreateTexture(renderer,
                                        SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        DISPLAY_WIDTH,
                                        DISPLAY_HEIGHT);

    if (!display_texture)
    {
        fprintf(stderr, "Texture creation failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Open shared memory for framebuffer */
    int shm_fb_fd = shm_open(SHM_FRAMEBUFFER, O_CREAT | O_RDWR, 0666);
    if (shm_fb_fd >= 0)
    {
        ftruncate(shm_fb_fd, DISPLAY_WIDTH * DISPLAY_HEIGHT * 2);
        framebuffer_shm = mmap(NULL, DISPLAY_WIDTH * DISPLAY_HEIGHT * 2,
                               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fb_fd, 0);
        if (framebuffer_shm == MAP_FAILED)
        {
            fprintf(stderr, "Warning: Framebuffer mmap failed\n");
            framebuffer_shm = NULL;
        }
        else
        {
            printf("✅ Framebuffer shared memory connected\n");
        }
    }

    /* Open shared memory for buttons */
    int shm_btn_fd = shm_open(SHM_BUTTONS, O_CREAT | O_RDWR, 0666);
    if (shm_btn_fd >= 0)
    {
        ftruncate(shm_btn_fd, sizeof(uint32_t));
        buttons_shm = mmap(NULL, sizeof(uint32_t),
                           PROT_READ | PROT_WRITE, MAP_SHARED, shm_btn_fd, 0);
        if (buttons_shm == MAP_FAILED)
        {
            fprintf(stderr, "Warning: Buttons mmap failed\n");
            buttons_shm = NULL;
        }
        else
        {
            *buttons_shm = 0;
            printf("✅ Button shared memory connected\n");
        }
    }

    printf("\n📺 Simulator window opened\n");
    printf("🎮 Controls:\n");
    printf("   WASD - D-Pad\n");
    printf("   IJKL - Action buttons (X/B/Y/A)\n");
    printf("   ESC  - Power\n");
    printf("   ENTER - Settings\n");
    printf("\n");

    /* Main loop */
    SDL_Event event;
    bool mouse_down = false;

    while (running)
    {
        /* Handle events */
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_QUIT:
                running = false;
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT)
                {
                    mouse_down = true;
                    handle_mouse(event.button.x, event.button.y, true);
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT)
                {
                    mouse_down = false;
                    handle_mouse(event.button.x, event.button.y, false);
                }
                break;

            case SDL_MOUSEMOTION:
                if (mouse_down)
                {
                    handle_mouse(event.motion.x, event.motion.y, true);
                }
                break;

            case SDL_KEYDOWN:
                handle_keyboard(event.key.keysym.sym, true);
                break;

            case SDL_KEYUP:
                handle_keyboard(event.key.keysym.sym, false);
                break;
            }
        }

        /* Render */
        SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
        SDL_RenderClear(renderer);

        /* Draw title bar */
        SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
        SDL_Rect title_bar = {0, 0, WINDOW_WIDTH, 40};
        SDL_RenderFillRect(renderer, &title_bar);

        render_display();
        render_buttons();

        SDL_RenderPresent(renderer);
        SDL_Delay(16); // 60 FPS
    }

    /* Cleanup */
    if (framebuffer_shm)
    {
        munmap(framebuffer_shm, DISPLAY_WIDTH * DISPLAY_HEIGHT * 2);
        shm_unlink(SHM_FRAMEBUFFER);
    }
    if (buttons_shm)
    {
        munmap(buttons_shm, sizeof(uint32_t));
        shm_unlink(SHM_BUTTONS);
    }

    SDL_DestroyTexture(display_texture);
    if (pcb_texture) SDL_DestroyTexture(pcb_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();

    printf("\n👋 Simulator closed\n");
    return 0;
}
