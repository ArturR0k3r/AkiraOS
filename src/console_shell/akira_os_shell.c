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
#include "wait_screen.h"

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
#if defined(CONFIG_AKIRA_USB)
#include <connectivity/usb/usb_manager.h>
#endif
#if defined(CONFIG_BT)
#include <connectivity/bluetooth/bt_manager.h>
#endif

/* For akira_power_should_stay_awake() and home_wake_isr() below. */
#include <zephyr/drivers/gpio.h>
#include <drivers/power/power_manager.h>
#include <storage/sd_card.h>
#if defined(CONFIG_AKIRA_MODULE_RF)
#include <api/akira_rf_api.h>
#endif
#if defined(CONFIG_AKIRA_WIFI_MANAGER)
#include <connectivity/wifi/wifi_manager.h>
#endif
#if defined(CONFIG_AKIRA_OTA) && defined(CONFIG_FLASH_MAP) && \
    defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <connectivity/ota/ota_manager.h>
#endif

/* True while a host management session is active over USB or BLE.  Deep sleep
 * powers down both the USB peripheral and the BT controller, so the device must
 * stay awake while either link is up — otherwise the web app's connection drops
 * out from under it.  Used to inhibit the wait-screen and deep-sleep paths. */
static bool host_session_active(void)
{
    bool active = false;
#if defined(CONFIG_AKIRA_USB)
    active = active || usb_manager_is_configured();
#endif
#if defined(CONFIG_BT)
    active = active || bt_manager_is_connected();
#endif
    return active;
}

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
static bool g_sd_popup_active;
static bool g_sd_popup_inserted;      /* true=insert, false=removal */
static bool g_sd_popup_apps_ready;    /* apps registered, dismiss when min time elapses */
static int g_sd_popup_dots;           /* 0-3 cycling dot count */
static int g_sd_popup_tick;           /* counts 20ms ticks for dot advance */
static int64_t g_sd_popup_dismiss_ms; /* non-zero = auto-dismiss at this uptime */
static int64_t g_sd_popup_shown_ms;   /* uptime when popup was shown */

#define SD_POPUP_DOT_TICKS 15   /* advance dots every 300ms */
#define SD_POPUP_REMOVE_MS 2000 /* removal popup duration */
#define SD_POPUP_MIN_MS 800     /* minimum visible time before insert popup dismisses */
#define SD_POPUP_W 240
#define SD_POPUP_H 70
#define SD_POPUP_X ((SCR_W - SD_POPUP_W) / 2)
#define SD_POPUP_Y ((SCR_H - SD_POPUP_H) / 2)

static void sd_popup_draw(void)
{
    int px = SD_POPUP_X;
    int py = SD_POPUP_Y;

    /* Panel: black bg + white double outline (install_progress style) */
    akira_display_rect(px, py, SD_POPUP_W, SD_POPUP_H, C_BLACK);
    akira_display_rect_outline(px, py, SD_POPUP_W, SD_POPUP_H, C_WHITE);
    akira_display_rect_outline(px + 1, py + 1, SD_POPUP_W - 2, SD_POPUP_H - 2, C_WHITE);

    /* Title */
    akira_display_text(px + 8, py + 8, "SD Card", C_WHITE);
    akira_display_hline(px + 8, py + 20, SD_POPUP_W - 16, C_WHITE);

    if (g_sd_popup_inserted)
    {
        char line[48];
        snprintf(line, sizeof(line), "Loading apps%.*s", g_sd_popup_dots, "...");
        akira_display_text(px + 8, py + 30, line, C_WHITE);
    }
    else
    {
        akira_display_text(px + 8, py + 30, "Removed", C_WHITE);
    }

    akira_display_flush();
}

static void sd_popup_show(bool inserted)
{
    g_sd_popup_active = true;
    g_sd_popup_inserted = inserted;
    g_sd_popup_dots = 0;
    g_sd_popup_tick = 0;
    g_sd_popup_shown_ms = k_uptime_get();
    g_sd_popup_dismiss_ms = inserted ? 0
                                     : (g_sd_popup_shown_ms + SD_POPUP_REMOVE_MS);
    sd_popup_draw();
}

static void sd_popup_dismiss(void)
{
    g_sd_popup_active = false;
    g_sd_popup_dismiss_ms = 0;
}

static void sd_popup_tick_fn(void)
{
    if (!g_sd_popup_active)
    {
        return;
    }

    int64_t now = k_uptime_get();

    /* Deferred dismiss: apps ready but min time hadn't elapsed yet */
    if (g_sd_popup_inserted && g_sd_popup_apps_ready &&
        (now - g_sd_popup_shown_ms) >= SD_POPUP_MIN_MS)
    {
        g_sd_popup_apps_ready = false;
        sd_popup_dismiss();
        home_screen_refresh();
        return;
    }

    /* Auto-dismiss removal popup */
    if (g_sd_popup_dismiss_ms && now >= g_sd_popup_dismiss_ms)
    {
        sd_popup_dismiss();
        home_screen_refresh();
        return;
    }

    /* Advance dots every SD_POPUP_DOT_TICKS × 20ms — redraw only on change */
    if (g_sd_popup_inserted)
    {
        g_sd_popup_tick++;
        if (g_sd_popup_tick >= SD_POPUP_DOT_TICKS)
        {
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

void akira_shell_abort_wasm_launch(void)
{
    g_wasm_active = false;
    akira_display_claim_shell();
}

/* ------------------------------------------------------------------ */
/* Shell main thread                                                   */
/* ------------------------------------------------------------------ */

#define SHELL_THREAD_STACK_SIZE 6144 /* trimmed from 8192 to reclaim SRAM for BT controller heap */
#define SHELL_THREAD_PRIORITY 10 /* above WASM apps (14), below sys work */

static K_THREAD_STACK_DEFINE(g_shell_stack, SHELL_THREAD_STACK_SIZE);
static struct k_thread g_shell_thread;

/* HOME button long-press threshold */
#define HOME_LONG_MS CONFIG_AKIRA_HOME_BUTTON_GPIO_LONG_MS

/* ------------------------------------------------------------------ */
/* HOME-button wake interrupt                                          */
/* ------------------------------------------------------------------ */
/* Lets the blanked-state loop block instead of poll at 50 Hz. gpio-keys
 * already owns HOME (sw0/GPIO0) with GPIO_INT_EDGE_BOTH; we only add a
 * second callback — reconfiguring the trigger would drop the release edge
 * and leave HOME reported as stuck held. ISR just posts the sem; the
 * existing 800 ms wake-hold still qualifies the press by polling. */
static K_SEM_DEFINE(home_wake_sem, 0, 1);
static const struct gpio_dt_spec home_wake_gpio =
    GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback home_wake_cb;

static void home_wake_isr(const struct device *port,
                          struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);
    k_sem_give(&home_wake_sem);
}

static void home_wake_intr_init(void)
{
    if (!device_is_ready(home_wake_gpio.port))
    {
        LOG_WRN("HOME wake GPIO not ready — blanked loop falls back to polling");
        return;
    }
    /* Register on the already-enabled EDGE_BOTH interrupt; do not reconfigure it. */
    gpio_init_callback(&home_wake_cb, home_wake_isr, BIT(home_wake_gpio.pin));
    gpio_add_callback(home_wake_gpio.port, &home_wake_cb);
}

#if defined(CONFIG_AKIRA_POWER_DEEP_SLEEP)
/* Deep sleep = sys_poweroff() = reboot. Refuse while anything is mid-job.
 * Poll if a subsystem already tracks its own busy-state correctly; only add
 * an akira_pm_insomnia_* lock if nothing else does. */
static bool akira_power_should_stay_awake(void)
{
    return host_session_active()                         /* USB configured or BT connected */
#if defined(CONFIG_AKIRA_POWER_MANAGER)
           /* Never power off a device sitting on a charger — the user plugged
            * it in expecting it to be there when they come back, and the
            * energy argument for sleeping does not apply on mains. */
           || akira_pm_is_charging()
#endif
#if defined(CONFIG_BT)
           || bt_manager_get_mode() != BT_MODE_NONE      /* BT scan/spam, not just connected */
#endif
#if defined(CONFIG_AKIRA_WIFI_MANAGER)
           || wifi_manager_get_state() != WIFI_MGR_STATE_IDLE
#endif
#if defined(CONFIG_AKIRA_OTA) && defined(CONFIG_FLASH_MAP) && \
    defined(CONFIG_BOOTLOADER_MCUBOOT)
           || ota_is_update_in_progress()
#endif
           || settings_screen_is_active()
           || g_wasm_active
           || akira_sd_card_is_transfer_active()         /* false fallback when SD absent */
           /* RF used to be tested here via akira_rf_daemon_is_running(), which
            * means "the daemon thread exists with a radio selected" — true
            * forever after the first `rf select`, permanently disabling auto
            * deep-sleep.  The RF layer now takes a normal insomnia lock (with a
            * deadline, so a leak self-heals), so it is covered by the count
            * below.  Prefer adding new keep-awake reasons the same way rather
            * than extending this predicate. */
           || akira_pm_insomnia_count() > 0;
}
#endif /* CONFIG_AKIRA_POWER_DEEP_SLEEP */

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

    home_wake_intr_init(); /* arm HOME wake so the blanked loop can block, not poll */

    /* Main event + render loop */
    static uint32_t s_prev_btns;
    static int64_t s_home_held_since_ms; /* 0 = not held */
    static bool s_home_fired;            /* true = fired this press, wait for release */

    /* Screen idle wait — load timeout from settings (0 = disabled) */
    int64_t s_display_timeout_ms = 60000; /* default 60 s — keeps battery healthy out of box */
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
            else
            {
                /* Key not set yet — use 60 s default */
                s_display_timeout_ms = 60000;
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
    /* Seconds on the wait screen before auto deep-sleep.  0 = never (the wait
     * screen stays a live clock forever); set via NVS akira/power/sleep_s,
     * defaulting to CONFIG_AKIRA_DEEP_SLEEP_IDLE_S when that key is unset.
     * Previously defaulted to 0, so a device left on a desk never slept. */
#if defined(CONFIG_AKIRA_POWER_DEEP_SLEEP)
    int s_deep_sleep_idle_s = CONFIG_AKIRA_DEEP_SLEEP_IDLE_S;
#else
    int s_deep_sleep_idle_s = 0;
#endif

    while (true)
    {
        uint32_t btns = akira_input_get_bitmask();
        int64_t now_ms = k_uptime_get();

        /* Edge detection — must be computed before any continue/branch that
         * might skip the s_prev_btns update inside the !g_wasm_active block.
         * Doing it here ensures s_prev_btns is always in sync every tick. */
        uint32_t just_pressed = btns & ~s_prev_btns;
        s_prev_btns = btns;

        /* Reset the idle timer on real press edges, but only while the screen
         * is awake.  Presses during sleep must not reset the timer — otherwise
         * tapping while on the wait screen would re-arm the idle countdown
         * (harmless today but semantically wrong and could mask future bugs). */
        if (just_pressed && !s_display_blanked)
        {
            s_last_input_ms = now_ms;
        }

        /* Phase 1 wake: require HOME held for CONFIG_AKIRA_WAIT_WAKE_HOLD_MS.
         * A plain tap is ignored — prevents accidental wakes from pocket
         * button contact.  We track level, not edge (just_pressed), so a
         * continuous hold is detected across loop iterations.
         *
         * Read the raw pin instead of the debounced akira_input_get_bitmask():
         * home_wake_isr wakes this thread the instant the edge fires, which is
         * faster than gpio-keys' debounce_interval_ms delay before it updates
         * the bitmask — btns here would still read the pre-press level, so the
         * hold timer would never start on a normal-length press. */
        static int64_t s_wake_hold_since_ms;
        if (s_display_blanked)
        {
            bool home_now = gpio_pin_get_dt(&home_wake_gpio) > 0;
            if (!home_now)
            {
                s_wake_hold_since_ms = 0;
            }
            else if (s_wake_hold_since_ms == 0)
            {
                s_wake_hold_since_ms = now_ms;
            }
            else if ((now_ms - s_wake_hold_since_ms) >=
                     CONFIG_AKIRA_WAIT_WAKE_HOLD_MS)
            {
                s_wake_hold_since_ms = 0;
                s_display_blanked = false;
                s_last_input_ms = now_ms; /* re-arm idle timer so we don't immediately re-blank */
                /* Suppress the HOME long-press that is currently in progress:
                 * the hold used to wake must not also fire CMD_GO_HOME.
                 * Mark it as already fired so the detector ignores it until
                 * the user releases and re-presses. */
                s_home_held_since_ms = 0;
                s_home_fired = true;
                wait_screen_exit(); /* -> notify_blank(false) */
                home_screen_refresh();
                s_prev_btns = akira_input_get_bitmask();
                k_sleep(K_MSEC(20));
                continue;
            }
        }

        /* HOME long-press detection — only while awake (sleep has its own
         * wake-hold path above that already consumes the button press). */
        bool home_held = !!(btns & BIT(AKIRA_BTN_HOME)) && !s_display_blanked;
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
            /* s_prev_btns and just_pressed already computed at loop top.
             * Guard with !s_display_blanked: keys must not reach the home or
             * settings screen while the sleep screen is up. */
            if (just_pressed && !s_display_blanked)
            {
                static const char *const btn_names[] = {
                    [AKIRA_BTN_HOME] = "HOME",
                    [AKIRA_BTN_UP] = "UP",
                    [AKIRA_BTN_DOWN] = "DOWN",
                    [AKIRA_BTN_LEFT] = "LEFT",
                    [AKIRA_BTN_RIGHT] = "RIGHT",
                    [AKIRA_BTN_A] = "A",
                    [AKIRA_BTN_B] = "B",
                    [AKIRA_BTN_X] = "X",
                    [AKIRA_BTN_Y] = "Y",
                };
                for (int _b = 0; _b < (int)ARRAY_SIZE(btn_names); _b++)
                {
                    if ((just_pressed & BIT(_b)) && btn_names[_b])
                    {
                        LOG_INF("BTN: %s", btn_names[_b]);
                    }
                }
                if (settings_screen_is_active())
                {
                    settings_screen_handle_key(just_pressed);
                }
                else
                {
                    home_screen_handle_key(just_pressed);
                }
            }

            /* Tick home screen animation every 20 ms (skip if SD popup active
             * or the idle wait screen is up — otherwise the home animation
             * repaints over the wait screen and flickers). */
            if (!s_display_blanked && !settings_screen_is_active()
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

            /* Status strip / wait screen updated every 1 s */
            static int64_t s_last_status_ms;
            if (now_ms - s_last_status_ms >= 1000)
            {
                s_last_status_ms = now_ms;
                if (s_display_blanked)
                {
                    wait_screen_update();
                }
                else if (settings_screen_is_active())
                {
                    settings_screen_update();
                }
                else
                {
                    home_screen_update_status();
                }

                /* Re-read the idle/sleep timeouts only when something actually
                 * wrote a setting, rather than on every 1 s tick.  Each
                 * akira_settings_get() is an NVS walk over flash — three of
                 * them per second, forever, for values that change maybe twice
                 * in the device's life.  The generation counter is bumped by
                 * the settings workqueue for every writer (shell, settings UI,
                 * HTTP, BLE companion), so nothing can change behind our back.
                 * Seeded to a value the counter cannot hold so the first tick
                 * always performs the initial read. */
#ifdef CONFIG_AKIRA_SETTINGS
                static unsigned int s_settings_gen_seen = (unsigned int)-1;
                unsigned int _gen_now = akira_settings_get_generation();
                if (!s_display_blanked && _gen_now != s_settings_gen_seen)
                {
                    s_settings_gen_seen = _gen_now;
                    bool _en = true;
                    char _sv[16] = "";
                    if (!akira_settings_get("akira/display/timeout_en",
                                            _sv, sizeof(_sv)))
                        _en = (atoi(_sv) != 0);
                    if (!_en)
                    {
                        s_display_timeout_ms = 0;
                    }
                    else
                    {
                        memset(_sv, 0, sizeof(_sv));
                        if (!akira_settings_get("akira/display/timeout_s",
                                                _sv, sizeof(_sv)))
                        {
                            int t = atoi(_sv);
                            s_display_timeout_ms =
                                (t > 0) ? (int64_t)t * 1000 : 0;
                        }
                        else
                        {
                            s_display_timeout_ms = 60000;
                        }
                    }

                    /* Auto deep-sleep timeout.  An explicit 0 from the user
                     * still means "never"; an *unset* key falls back to the
                     * build default rather than to never. */
                    memset(_sv, 0, sizeof(_sv));
                    if (!akira_settings_get("akira/power/sleep_s",
                                            _sv, sizeof(_sv)))
                    {
                        int t = atoi(_sv);
                        s_deep_sleep_idle_s = (t > 0) ? t : 0;
                    }
                    else
                    {
#if defined(CONFIG_AKIRA_POWER_DEEP_SLEEP)
                        s_deep_sleep_idle_s = CONFIG_AKIRA_DEEP_SLEEP_IDLE_S;
#else
                        s_deep_sleep_idle_s = 0;
#endif
                    }
                }
#endif

                /* Keep awake while a USB/BLE host session is active: treat the
                 * live link as continuous input so the wait screen never
                 * engages, and if a host connects while already blanked, wake
                 * the device so the radios stay powered. */
                if (host_session_active())
                {
                    s_last_input_ms = now_ms;
                    if (s_display_blanked)
                    {
                        s_display_blanked = false;
                        wait_screen_exit(); /* -> notify_blank(false) */
                        home_screen_refresh();
                        s_prev_btns = akira_input_get_bitmask();
                    }
                }

                /* Idle wait-screen check */
                static int64_t s_deep_sleep_arm_ms;
                if (!s_display_blanked && s_display_timeout_ms > 0 &&
                    (now_ms - s_last_input_ms) >= s_display_timeout_ms)
                {
                    s_display_blanked = true;
                    s_deep_sleep_arm_ms = now_ms;
                    wait_screen_enter(); /* -> notify_blank(true) */
                    /* Drain button noise accumulated during wait_screen_enter's
                     * I2C + SPI flush.  Without this, the next loop iteration
                     * computes just_pressed against s_prev_btns=0, sees the
                     * noise bits as a fresh edge, and immediately wakes. */
                    s_prev_btns = akira_input_get_bitmask();
                }
                if (!s_display_blanked)
                {
                    s_deep_sleep_arm_ms = 0;
                }
#ifdef CONFIG_AKIRA_POWER_DEEP_SLEEP
                /* Auto deep-sleep only when a sleep timeout is configured
                 * (akira/power/sleep_s > 0). Default 0 = never: the wait screen
                 * keeps running as a live clock (CPU idle, redrawn each minute).
                 * Manual power-off (web app / power button) still sleeps. */
                if (s_display_blanked && s_deep_sleep_arm_ms &&
                    s_deep_sleep_idle_s > 0 &&
                    !akira_power_should_stay_awake() &&
                    (now_ms - s_deep_sleep_arm_ms) >=
                        (int64_t)s_deep_sleep_idle_s * 1000)
                {
                    wait_screen_prepare_deep_sleep(); /* normally does not return */
                    /* If PM is a no-op (CONFIG_PM=n) the call returns.  Re-arm
                     * so we don't hammer it every second — wait another full
                     * delay before trying again. */
                    s_deep_sleep_arm_ms = now_ms;
                }
#endif
            }

            /* Timeout is re-read from NVS in the 1-second tick above, so
             * settings and shell changes take effect within 1 s automatically.
             * No extra per-frame re-read needed here. */

            /* Blanked + HOME up: block on the sem until either the HOME ISR
             * fires or the wait screen actually needs a repaint (minute
             * rollover), capped so the host-session and deep-sleep checks above
             * still run.  This is the state the device spends most of its life
             * in, so the wake rate here dominates standby battery life —
             * previously a fixed 1 Hz for a frame that changes once a minute.
             * HOME down falls through to the 20ms poll so the
             * CONFIG_AKIRA_WAIT_WAKE_HOLD_MS hold detector stays accurate.
             * Gate on the raw pin, not btns: btns lags by debounce_interval_ms,
             * so right after the press edge this would still see HOME up and
             * re-enter the multi-second sem block with nothing but the release
             * edge left to wake it — starving the hold detector of every
             * iteration until it's too late to reach the threshold. */
            if (s_display_blanked && gpio_pin_get_dt(&home_wake_gpio) <= 0)
            {
                uint32_t idle_ms = wait_screen_ms_to_next_update();
                if (idle_ms > WAIT_SCREEN_UPDATE_MAX_MS)
                {
                    idle_ms = WAIT_SCREEN_UPDATE_MAX_MS;
                }
                k_sem_take(&home_wake_sem, K_MSEC(idle_ms));
            }
            else
            {
                /* Awake tick.  Button input is interrupt-driven (see
                 * akira_input_api.c), so this rate only sets animation
                 * smoothness and the granularity of the HOME hold detector —
                 * 30 Hz is imperceptible for both and is a third fewer wakes
                 * than the old 50 Hz. */
                k_sleep(K_MSEC(33));
            }
        }
        else
        {
            /* Shell dormant while WASM holds display.
             * s_prev_btns is already updated at the top of the loop. */
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

                /* Settings→Sleep blanks the panel independently of the shell's
                 * own s_display_blanked/wait_screen state, so HOME's long-press
                 * wake must also unblank it and close the settings screen here —
                 * otherwise the panel stays blanked and input keeps routing to
                 * the sleep page's own B-B wake gesture. */
                if (settings_screen_is_sleeping())
                {
                    settings_screen_wake();
                }

                /* Reclaim display immediately so the home screen is visible
                 * before app_manager_stop() blocks for the abort timeout. */
                g_wasm_active = false;
                akira_display_claim_shell();
                s_prev_btns = akira_input_get_bitmask();
                home_screen_load();

                /* Stop any running WASM apps (may block up to abort timeout) */
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
                }
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
                        /* All apps stopped — reclaim display.
                         * Resync s_prev_btns to the button state at the moment
                         * of reclaim: if the app exited because the user is
                         * still holding a button (e.g. A), the home screen
                         * must not see that same physical press as a fresh
                         * "just pressed" edge on the next tick. */
                        g_wasm_active = false;
                        akira_display_claim_shell();
                        s_prev_btns = akira_input_get_bitmask();
                        home_screen_refresh();
                        LOG_INF("Display reclaimed by shell (all apps stopped)");
                    }
                    else
                    {
                        if (!g_wasm_active)
                        {
#ifdef CONFIG_AKIRA_SD_HOTPLUG
                            if (g_sd_popup_active && g_sd_popup_inserted)
                            {
                                if ((k_uptime_get() - g_sd_popup_shown_ms) >= SD_POPUP_MIN_MS)
                                {
                                    sd_popup_dismiss();
                                }
                                else
                                {
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
                if (!g_wasm_active)
                {
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
    ARG_UNUSED(user_data);
    ARG_UNUSED(present);
    shell_event_t ev = {.type = CMD_SD_CARD_EVENT, .sd = {.present = true}};
    k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
}

/* Post-hotplug: fires after init+deinit — show removal popup */
static void shell_sd_hotplug_cb(bool present, void *user_data)
{
    ARG_UNUSED(user_data);
    if (!present)
    {
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
