/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_os_shell
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_os_shell, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_os_shell.c
 * @brief AkiraConsole Native OS Shell — core thread and event dispatcher.
 *
 * Boot order:
 *   1. SYS_INIT(APPLICATION) → shell_init() starts the shell thread.
 *   2. Shell thread claims the display, builds the HOME screen, and enters
 *      the main event loop.
 *   3. When the user launches a WASM app the shell releases the display.
 *   4. When the user long-presses (HOME) the shell loop detects the hold
 *      duration and enqueues CMD_GO_HOME.
 *   5. Shell thread stops the active WASM app, reclaims the display, and
 *      refreshes the HOME screen.
 *
 * The shell also listens on the "akira.lifecycle" IPC topic to keep app
 * state dots up-to-date without polling.
 *
 * Install progress events from BLE/HTTP are delivered through
 * akira_os_shell_notify_install_progress() → g_install_progress_msgq.
 */

#include "akira_os_shell.h"
#include "display_ownership.h"
#include "home_screen.h"
#include "settings_screen.h"
#include "shell_theme.h"
#include "install_progress_screen.h"

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "drivers/platform_hal.h"
#include <api/akira_display_api.h>
#ifdef CONFIG_AKIRA_SD_HOTPLUG
#include <storage/sd_card.h>
#endif

#if defined(CONFIG_DISPLAY)
#include <zephyr/drivers/display.h>
#if defined(CONFIG_AKIRA_BOOT_ANIMATION)
#include "boot_anim.h"
#endif
#endif

#include <runtime/akira_ipc.h>
#include <runtime/app_manager/app_manager.h>

#include <api/akira_input_api.h>
#ifdef CONFIG_AKIRA_SETTINGS
#include <settings/settings.h>
#endif

typedef enum
{
    CMD_GO_HOME = 0,       /* Return to HOME screen (reclaim display) */
    CMD_APP_STATE_CHANGED, /* App lifecycle changed — refresh home list */
    CMD_INSTALL_PROGRESS,  /* Install progress update */
    CMD_SD_CARD_EVENT,     /* SD card inserted/removed */
} shell_cmd_t;

typedef struct
{
    shell_cmd_t type;
    union
    {
        /* CMD_INSTALL_PROGRESS */
        struct
        {
            char name[APP_NAME_MAX_LEN];
            int pct;
            char msg[64];
        } install;
        /* CMD_SD_CARD_EVENT */
        struct
        {
            bool present;
        } sd;
    };
} shell_event_t;

/* ------------------------------------------------------------------ */
/* Message queues                                                      */
/* ------------------------------------------------------------------ */

#define SHELL_EVENT_QUEUE_DEPTH 8

K_MSGQ_DEFINE(g_shell_msgq,
              sizeof(shell_event_t),
              SHELL_EVENT_QUEUE_DEPTH,
              4);

/* ------------------------------------------------------------------ */
/* Shell state                                                         */
/* ------------------------------------------------------------------ */

static bool g_wasm_active; /* true while a WASM app has the display */

/* SD popup state */
static bool    g_sd_popup_active;
static bool    g_sd_popup_inserted;   /* true=insert, false=removal */
static bool    g_sd_popup_apps_ready; /* apps registered, dismiss when min time elapses */
static int     g_sd_popup_dots;     /* 0-3 cycling dot count */
static int     g_sd_popup_tick;     /* counts 20ms ticks for dot advance */
static int64_t g_sd_popup_dismiss_ms; /* non-zero = auto-dismiss at this uptime */
static int64_t g_sd_popup_shown_ms;   /* uptime when popup was shown */

#define SD_POPUP_DOT_TICKS  15   /* advance dots every 300ms */
#define SD_POPUP_REMOVE_MS  2000 /* removal popup duration */
#define SD_POPUP_MIN_MS     800  /* minimum visible time before insert popup dismisses */
#define SD_POPUP_W          240
#define SD_POPUP_H          70
#define SD_POPUP_X          ((SCR_W - SD_POPUP_W) / 2)
#define SD_POPUP_Y          ((SCR_H - SD_POPUP_H) / 2)

static void sd_popup_draw(void)
{
    int px = SD_POPUP_X;
    int py = SD_POPUP_Y;

    /* Panel: black bg + white double outline (install_progress style) */
    akira_display_rect(px, py, SD_POPUP_W, SD_POPUP_H, C_BLACK);
    akira_display_rect_outline(px,     py,     SD_POPUP_W,     SD_POPUP_H,     C_WHITE);
    akira_display_rect_outline(px + 1, py + 1, SD_POPUP_W - 2, SD_POPUP_H - 2, C_WHITE);

    /* Title */
    akira_display_text(px + 8, py + 8, "SD Card", C_WHITE);
    akira_display_hline(px + 8, py + 20, SD_POPUP_W - 16, C_WHITE);

    if (g_sd_popup_inserted) {
        char line[48];
        snprintf(line, sizeof(line), "Loading apps%.*s", g_sd_popup_dots, "...");
        akira_display_text(px + 8, py + 30, line, C_WHITE);
    } else {
        akira_display_text(px + 8, py + 30, "Removed", C_WHITE);
    }

    akira_display_flush();
}

static void sd_popup_show(bool inserted)
{
    g_sd_popup_active    = true;
    g_sd_popup_inserted  = inserted;
    g_sd_popup_dots      = 0;
    g_sd_popup_tick      = 0;
    g_sd_popup_shown_ms  = k_uptime_get();
    g_sd_popup_dismiss_ms = inserted ? 0
                          : (g_sd_popup_shown_ms + SD_POPUP_REMOVE_MS);
    sd_popup_draw();
}

static void sd_popup_dismiss(void)
{
    g_sd_popup_active     = false;
    g_sd_popup_dismiss_ms = 0;
}

static void sd_popup_tick_fn(void)
{
    if (!g_sd_popup_active) {
        return;
    }

    int64_t now = k_uptime_get();

    /* Deferred dismiss: apps ready but min time hadn't elapsed yet */
    if (g_sd_popup_inserted && g_sd_popup_apps_ready &&
        (now - g_sd_popup_shown_ms) >= SD_POPUP_MIN_MS) {
        g_sd_popup_apps_ready = false;
        sd_popup_dismiss();
        home_screen_refresh();
        return;
    }

    /* Auto-dismiss removal popup */
    if (g_sd_popup_dismiss_ms && now >= g_sd_popup_dismiss_ms) {
        sd_popup_dismiss();
        home_screen_refresh();
        return;
    }

    /* Advance dots every SD_POPUP_DOT_TICKS × 20ms — redraw only on change */
    if (g_sd_popup_inserted) {
        g_sd_popup_tick++;
        if (g_sd_popup_tick >= SD_POPUP_DOT_TICKS) {
            g_sd_popup_tick = 0;
            g_sd_popup_dots = (g_sd_popup_dots + 1) % 4;
            sd_popup_draw();
        }
    }
}

/* ------------------------------------------------------------------ */
/* IPC lifecycle listener thread                                       */
/* ------------------------------------------------------------------ */

#define LIFECYCLE_LISTENER_STACK 1024
#define LIFECYCLE_TOPIC "akira.lifecycle"
#define LIFECYCLE_SUBSCRIBER "akira_os_shell"

static K_THREAD_STACK_DEFINE(g_lifecycle_stack, LIFECYCLE_LISTENER_STACK);
static struct k_thread g_lifecycle_thread;

static void lifecycle_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    int ret = akira_ipc_subscribe(LIFECYCLE_TOPIC, LIFECYCLE_SUBSCRIBER);
    if (ret < 0)
    {
        LOG_ERR("Failed to subscribe to lifecycle topic: %d", ret);
        return;
    }

    uint8_t buf[CONFIG_AKIRA_IPC_MSG_MAX_SIZE];

    while (true)
    {
        int n = akira_ipc_recv(LIFECYCLE_TOPIC, LIFECYCLE_SUBSCRIBER,
                               buf, sizeof(buf), K_FOREVER);
        if (n < 0)
        {
            LOG_DBG("lifecycle recv error: %d", n);
            continue;
        }

        shell_event_t ev = {.type = CMD_APP_STATE_CHANGED};
        k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
    }
}

/* ------------------------------------------------------------------ */
/* Public: install progress notification                               */
/* ------------------------------------------------------------------ */

void akira_os_shell_notify_install_progress(const char *name, int pct,
                                            const char *msg)
{
    shell_event_t ev = {.type = CMD_INSTALL_PROGRESS};

    if (name)
    {
        strncpy(ev.install.name, name, sizeof(ev.install.name) - 1);
    }
    ev.install.pct = pct;
    if (msg)
    {
        strncpy(ev.install.msg, msg, sizeof(ev.install.msg) - 1);
    }

    k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
}

void akira_os_shell_go_home(void)
{
    shell_event_t ev = {.type = CMD_GO_HOME};
    k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
}

void akira_os_shell_notify_app_changed(void)
{
    shell_event_t ev = {.type = CMD_APP_STATE_CHANGED};
    k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
}

void akira_shell_set_wasm_launching(void)
{
    akira_display_clear(C_BLACK);
    akira_display_flush();
    g_wasm_active = true;
    akira_display_release_to_wasm();
}

/* ------------------------------------------------------------------ */
/* Shell main thread                                                   */
/* ------------------------------------------------------------------ */

#define SHELL_THREAD_STACK_SIZE 4096 * 2
#define SHELL_THREAD_PRIORITY 10 /* above WASM apps (14), below sys work */

static K_THREAD_STACK_DEFINE(g_shell_stack, SHELL_THREAD_STACK_SIZE);
static struct k_thread g_shell_thread;

/* HOME button long-press threshold */
#define HOME_LONG_MS CONFIG_AKIRA_HOME_BUTTON_GPIO_LONG_MS

static void shell_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    /* Claim display — shell owns it from boot */
    int ret = akira_display_claim_shell();
    if (ret < 0)
    {
        LOG_ERR("Shell failed to claim display at boot: %d", ret);
        return;
    }

#if defined(CONFIG_DISPLAY)
    /* Enable display output (ST7789V starts blanked) */
    const struct device *_disp_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (device_is_ready(_disp_dev))
    {
        display_blanking_off(_disp_dev);
        LOG_INF("Display enabled");
#if defined(CONFIG_AKIRA_BOOT_ANIMATION)
        boot_anim_run();
#endif
    }
    else
    {
        LOG_WRN("Display device not ready");
    }
#endif

    /* Build the HOME app launcher screen (pure akira_display_* renderer) */
#if defined(CONFIG_LVGL)
    shell_theme_init();
#endif
    home_screen_create();
    settings_screen_create();
    home_screen_refresh();

    /* Start the IPC lifecycle listener thread */
    k_thread_create(&g_lifecycle_thread,
                    g_lifecycle_stack,
                    K_THREAD_STACK_SIZEOF(g_lifecycle_stack),
                    lifecycle_thread_fn, NULL, NULL, NULL,
                    14, 0, K_NO_WAIT);
    k_thread_name_set(&g_lifecycle_thread, "shell_lifecycle");

    /* Main event + render loop */
    static uint32_t s_prev_btns;
    static int64_t s_home_held_since_ms; /* 0 = not held */
    static bool    s_home_fired;         /* true = fired this press, wait for release */

    /* Screen idle-blank — load timeout from settings (0 = disabled) */
    int64_t s_display_timeout_ms = 60000; /* default 60 s */
#ifdef CONFIG_AKIRA_SETTINGS
    {
        bool en = true;
        char _sv[16] = "";
        if (!akira_settings_get("akira/display/timeout_en", _sv, sizeof(_sv)))
            en = (atoi(_sv) != 0);
        if (en)
        {
            memset(_sv, 0, sizeof(_sv));
            if (!akira_settings_get("akira/display/timeout_s", _sv, sizeof(_sv)))
            {
                int t = atoi(_sv);
                s_display_timeout_ms = (t > 0) ? (int64_t)t * 1000 : 0;
            }
        }
        else
        {
            s_display_timeout_ms = 0;
        }
    }
#endif
    int64_t s_last_input_ms = k_uptime_get();
    bool s_display_blanked = false;

    while (true)
    {
        uint32_t btns = akira_input_get_bitmask();
        int64_t now_ms = k_uptime_get();

        /* Any button activity resets the idle timer */
        if (btns)
        {
            s_last_input_ms = now_ms;
        }

        /* Wake display if blanked and a button was just pressed */
        if (s_display_blanked && btns)
        {
            s_display_blanked = false;
            akira_display_hal_set_blank(false);
#ifdef CONFIG_AKIRA_SETTINGS
            {
                char _sv[16] = "";
                if (!akira_settings_get("akira/display/brightness", _sv, sizeof(_sv)))
                {
                    akira_display_hal_set_brightness((uint8_t)(atoi(_sv) * 255 / 100));
                }
            }
#endif
            /* Swallow this press so navigation doesn't fire while waking */
            s_prev_btns = btns;
            k_sleep(K_MSEC(20));
            continue;
        }

        /* HOME long-press detection — works regardless of display owner */
        bool home_held = !!(btns & BIT(AKIRA_BTN_HOME));
        if (!home_held)
        {
            s_home_held_since_ms = 0;
            s_home_fired = false;
        }
        else if (!s_home_fired && s_home_held_since_ms == 0)
        {
            s_home_held_since_ms = now_ms;
        }
        else if (!s_home_fired && s_home_held_since_ms &&
                 (now_ms - s_home_held_since_ms) >= HOME_LONG_MS)
        {
            /* Fire once — s_home_fired blocks re-trigger until button released */
            s_home_fired = true;
            shell_event_t ev = {.type = CMD_GO_HOME};
            k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
        }

        if (!g_wasm_active)
        {
            /* Button edge detection → active screen navigation */
            uint32_t just = btns & ~s_prev_btns;
            s_prev_btns = btns;
            if (just)
            {
                static const char *const btn_names[] = {
                    [AKIRA_BTN_HOME]  = "HOME",
                    [AKIRA_BTN_UP]    = "UP",
                    [AKIRA_BTN_DOWN]  = "DOWN",
                    [AKIRA_BTN_LEFT]  = "LEFT",
                    [AKIRA_BTN_RIGHT] = "RIGHT",
                    [AKIRA_BTN_A]     = "A",
                    [AKIRA_BTN_B]     = "B",
                    [AKIRA_BTN_X]     = "X",
                    [AKIRA_BTN_Y]     = "Y",
                };
                for (int _b = 0; _b < (int)ARRAY_SIZE(btn_names); _b++) {
                    if ((just & BIT(_b)) && btn_names[_b]) {
                        LOG_INF("BTN: %s", btn_names[_b]);
                    }
                }
                if (settings_screen_is_active())
                {
                    settings_screen_handle_key(just);
                }
                else
                {
                    home_screen_handle_key(just);
                }
            }

            /* Tick home screen animation every 20 ms (skip if SD popup active) */
            if (!settings_screen_is_active()
#ifdef CONFIG_AKIRA_SD_HOTPLUG
                && !g_sd_popup_active
#endif
                )
            {
                home_screen_tick();
            }

#ifdef CONFIG_AKIRA_SD_HOTPLUG
            sd_popup_tick_fn();
#endif

            /* Status strip updated every 1 s */
            static int64_t s_last_status_ms;
            if (now_ms - s_last_status_ms >= 1000)
            {
                s_last_status_ms = now_ms;
                if (settings_screen_is_active())
                {
                    settings_screen_update();
                }
                else
                {
                    home_screen_update_status();
                }

                /* Idle screen-off check */
                if (!s_display_blanked && s_display_timeout_ms > 0 &&
                    (now_ms - s_last_input_ms) >= s_display_timeout_ms)
                {
                    s_display_blanked = true;
                    akira_display_hal_set_blank(true);
                }

                /* Re-read timeout in case the user just changed it in settings */
#ifdef CONFIG_AKIRA_SETTINGS
                {
                    bool en = true;
                    char _sv[16] = "";
                    if (!akira_settings_get("akira/display/timeout_en", _sv, sizeof(_sv)))
                        en = (atoi(_sv) != 0);
                    if (en)
                    {
                        memset(_sv, 0, sizeof(_sv));
                        if (!akira_settings_get("akira/display/timeout_s", _sv, sizeof(_sv)))
                        {
                            int t = atoi(_sv);
                            s_display_timeout_ms = (t > 0) ? (int64_t)t * 1000 : 0;
                        }
                    }
                    else
                    {
                        s_display_timeout_ms = 0;
                    }
                }
#endif
            }

            k_sleep(K_MSEC(20));
        }
        else
        {
            /* Shell dormant while WASM holds display.
             * Keep s_prev_btns current so edge detection is clean
             * when the app exits and shell reclaims the display. */
            s_prev_btns = btns;
            k_sleep(K_MSEC(50));
        }

        /* Drain the event queue */
        shell_event_t ev;
        while (k_msgq_get(&g_shell_msgq, &ev, K_NO_WAIT) == 0)
        {
            switch (ev.type)
            {

            case CMD_GO_HOME:
                LOG_INF("HOME pressed — returning to launcher");

                /* Stop any running WASM app */
                if (g_wasm_active)
                {
                    app_info_t apps[CONFIG_AKIRA_APP_MAX_INSTALLED];
                    int n = app_manager_list(apps,
                                             ARRAY_SIZE(apps));
                    for (int i = 0; i < n; i++)
                    {
                        if (apps[i].state == APP_STATE_RUNNING)
                        {
                            app_manager_stop(apps[i].name);
                        }
                    }
                    g_wasm_active = false;
                }

                /* Reclaim display for shell — dismiss settings if open */
                akira_display_claim_shell();
                home_screen_load();
                break;

            case CMD_APP_STATE_CHANGED:
                /* An app launched or stopped — check if WASM holds display */
                {
                    app_info_t apps[CONFIG_AKIRA_APP_MAX_INSTALLED];
                    int n = app_manager_list(apps,
                                             ARRAY_SIZE(apps));
                    bool any_running = false;

                    for (int i = 0; i < n; i++)
                    {
                        if (apps[i].state == APP_STATE_RUNNING)
                        {
                            any_running = true;
                            break;
                        }
                    }

                    if (any_running && !g_wasm_active)
                    {
                        /* App just started — release display to it */
                        g_wasm_active = true;
                        akira_display_release_to_wasm();
                        LOG_INF("Display released to WASM app");
                    }
                    else if (!any_running && g_wasm_active)
                    {
                        /* All apps stopped — reclaim display */
                        g_wasm_active = false;
                        akira_display_claim_shell();
                        home_screen_refresh();
                        LOG_INF("Display reclaimed by shell (all apps stopped)");
                    }
                    else
                    {
                        if (!g_wasm_active)
                        {
#ifdef CONFIG_AKIRA_SD_HOTPLUG
                            if (g_sd_popup_active && g_sd_popup_inserted) {
                                if ((k_uptime_get() - g_sd_popup_shown_ms) >= SD_POPUP_MIN_MS) {
                                    sd_popup_dismiss();
                                } else {
                                    /* Too soon — let tick dismiss once min time passes */
                                    g_sd_popup_apps_ready = true;
                                }
                            }
#endif
                            home_screen_refresh();
                        }
                    }
                }
                break;

            case CMD_INSTALL_PROGRESS:
                if (!g_wasm_active)
                {
                    install_progress_show(ev.install.name,
                                          ev.install.pct,
                                          ev.install.msg);
                }
                break;

#ifdef CONFIG_AKIRA_SD_HOTPLUG
            case CMD_SD_CARD_EVENT:
                if (!g_wasm_active) {
                    sd_popup_show(ev.sd.present);
                }
                break;
#endif

            default:
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* SYS_INIT registration                                               */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_AKIRA_SD_HOTPLUG
/* Pre-insert: fires before init — show loading popup immediately */
static void shell_sd_pre_insert_cb(bool present, void *user_data)
{
    ARG_UNUSED(user_data); ARG_UNUSED(present);
    shell_event_t ev = {.type = CMD_SD_CARD_EVENT, .sd = {.present = true}};
    k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
}

/* Post-hotplug: fires after init+deinit — show removal popup */
static void shell_sd_hotplug_cb(bool present, void *user_data)
{
    ARG_UNUSED(user_data);
    if (!present) {
        shell_event_t ev = {.type = CMD_SD_CARD_EVENT, .sd = {.present = false}};
        k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
    }
    /* Insert: popup already shown via pre_insert_cb; dismiss handled by
     * CMD_APP_STATE_CHANGED when sd_manager finishes registering apps. */
}
#endif

static int shell_init(void)
{
#ifdef CONFIG_AKIRA_SD_HOTPLUG
    akira_sd_card_register_pre_insert_cb(shell_sd_pre_insert_cb, NULL);
    akira_sd_card_register_hotplug_cb(shell_sd_hotplug_cb, NULL);
#endif

    k_thread_create(&g_shell_thread,
                    g_shell_stack,
                    K_THREAD_STACK_SIZEOF(g_shell_stack),
                    shell_thread_fn, NULL, NULL, NULL,
                    SHELL_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&g_shell_thread, "akira_os_shell");

    LOG_INF("AkiraOS Shell started");
    return 0;
}

SYS_INIT(shell_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
