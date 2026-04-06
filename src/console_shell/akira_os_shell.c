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
 *   4. When the user long-presses X (HOME) the shell loop detects the hold
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

#if defined(CONFIG_DISPLAY)
#include <zephyr/drivers/display.h>
#endif

#include <runtime/akira_ipc.h>
#include <runtime/app_manager/app_manager.h>

#include <api/akira_input_api.h>

typedef enum {
    CMD_GO_HOME = 0,        /* Return to HOME screen (reclaim display) */
    CMD_APP_STATE_CHANGED,  /* App lifecycle changed — refresh home list */
    CMD_INSTALL_PROGRESS,   /* Install progress update */
} shell_cmd_t;

typedef struct {
    shell_cmd_t type;
    union {
        /* CMD_INSTALL_PROGRESS */
        struct {
            char name[APP_NAME_MAX_LEN];
            int  pct;
            char msg[64];
        } install;
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

/* ------------------------------------------------------------------ */
/* IPC lifecycle listener thread                                       */
/* ------------------------------------------------------------------ */

#define LIFECYCLE_LISTENER_STACK  1024
#define LIFECYCLE_TOPIC           "akira.lifecycle"
#define LIFECYCLE_SUBSCRIBER      "akira_os_shell"

static K_THREAD_STACK_DEFINE(g_lifecycle_stack, LIFECYCLE_LISTENER_STACK);
static struct k_thread g_lifecycle_thread;

static void lifecycle_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    int ret = akira_ipc_subscribe(LIFECYCLE_TOPIC, LIFECYCLE_SUBSCRIBER);
    if (ret < 0) {
        LOG_ERR("Failed to subscribe to lifecycle topic: %d", ret);
        return;
    }

    uint8_t buf[CONFIG_AKIRA_IPC_MSG_MAX_SIZE];

    while (true) {
        int n = akira_ipc_recv(LIFECYCLE_TOPIC, LIFECYCLE_SUBSCRIBER,
                               buf, sizeof(buf), K_FOREVER);
        if (n < 0) {
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

    if (name) {
        strncpy(ev.install.name, name, sizeof(ev.install.name) - 1);
    }
    ev.install.pct = pct;
    if (msg) {
        strncpy(ev.install.msg, msg, sizeof(ev.install.msg) - 1);
    }

    k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
}

void akira_os_shell_go_home(void)
{
    shell_event_t ev = {.type = CMD_GO_HOME};
    k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
}

/* ------------------------------------------------------------------ */
/* Shell main thread                                                   */
/* ------------------------------------------------------------------ */

#define SHELL_THREAD_STACK_SIZE  4096
#define SHELL_THREAD_PRIORITY    10  /* above WASM apps (14), below sys work */

static K_THREAD_STACK_DEFINE(g_shell_stack, SHELL_THREAD_STACK_SIZE);
static struct k_thread g_shell_thread;

/* HOME (X) button long-press threshold */
#define HOME_LONG_MS  CONFIG_AKIRA_HOME_BUTTON_GPIO_LONG_MS

static void shell_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    /* Claim display — shell owns it from boot */
    int ret = akira_display_claim_shell();
    if (ret < 0) {
        LOG_ERR("Shell failed to claim display at boot: %d", ret);
        return;
    }

#if defined(CONFIG_DISPLAY)
    /* Enable display output (ST7789V starts blanked) */
    const struct device *_disp_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (device_is_ready(_disp_dev)) {
        display_blanking_off(_disp_dev);
        LOG_INF("Display enabled");
    } else {
        LOG_WRN("Display device not ready");
    }
#endif

    /* Build the HOME app launcher screen (pure akira_display_* renderer) */
    shell_theme_init();
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
    static int64_t  s_home_held_since_ms; /* 0 = not held */

    while (true) {
        uint32_t btns = akira_input_get_bitmask();
        int64_t  now_ms = k_uptime_get();

        /* HOME (X) long-press detection — works regardless of display owner */
        bool home_held = !!(btns & BIT(AKIRA_BTN_X));
        if (home_held && s_home_held_since_ms == 0) {
            s_home_held_since_ms = now_ms;
        } else if (!home_held) {
            s_home_held_since_ms = 0;
        } else if (s_home_held_since_ms &&
                   (now_ms - s_home_held_since_ms) >= HOME_LONG_MS) {
            /* Long-press threshold crossed — fire once then reset */
            s_home_held_since_ms = 0;
            shell_event_t ev = {.type = CMD_GO_HOME};
            k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
        }

        if (!g_wasm_active) {
            /* Button edge detection → active screen navigation */
            uint32_t just = btns & ~s_prev_btns;
            s_prev_btns = btns;
            if (just) {
                if (settings_screen_is_active()) {
                    settings_screen_handle_key(just);
                } else {
                    home_screen_handle_key(just);
                }
            }

            /* Status strip updated every 1 s */
            static int64_t s_last_status_ms;
            if (now_ms - s_last_status_ms >= 1000) {
                s_last_status_ms = now_ms;
                if (settings_screen_is_active()) {
                    settings_screen_update();
                } else {
                    home_screen_update_status();
                }
            }

            k_sleep(K_MSEC(20));
        } else {
            /* Shell dormant while WASM holds display */
            k_sleep(K_MSEC(50));
        }

        /* Drain the event queue */
        shell_event_t ev;
        while (k_msgq_get(&g_shell_msgq, &ev, K_NO_WAIT) == 0) {
            switch (ev.type) {

            case CMD_GO_HOME:
                LOG_INF("HOME pressed — returning to launcher");

                /* Stop any running WASM app */
                if (g_wasm_active) {
                    app_info_t apps[CONFIG_AKIRA_APP_MAX_INSTALLED];
                    int n = app_manager_list(apps,
                                ARRAY_SIZE(apps));
                    for (int i = 0; i < n; i++) {
                        if (apps[i].state == APP_STATE_RUNNING) {
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

                    for (int i = 0; i < n; i++) {
                        if (apps[i].state == APP_STATE_RUNNING) {
                            any_running = true;
                            break;
                        }
                    }

                    if (any_running && !g_wasm_active) {
                        /* App just started — release display to it */
                        g_wasm_active = true;
                        akira_display_release_to_wasm();
                        LOG_INF("Display released to WASM app");
                    } else if (!any_running && g_wasm_active) {
                        /* All apps stopped — reclaim display */
                        g_wasm_active = false;
                        akira_display_claim_shell();
                        home_screen_refresh();
                        LOG_INF("Display reclaimed by shell (all apps stopped)");
                    } else {
                        /* Refresh status dots without changing ownership */
                        if (!g_wasm_active) {
                            home_screen_refresh();
                        }
                    }
                }
                break;

            case CMD_INSTALL_PROGRESS:
                if (!g_wasm_active) {
                    install_progress_show(ev.install.name,
                                          ev.install.pct,
                                          ev.install.msg);
                }
                break;

            default:
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* SYS_INIT registration                                               */
/* ------------------------------------------------------------------ */

static int shell_init(void)
{
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
