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
 *   2. Shell thread claims the display, puts the HOME screen at the root of
 *      the screen stack (ui/screen.c) and enters the main event loop.
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
 * akira_os_shell_notify_install_progress() → g_shell_msgq, battery events
 * from the power manager's monitor the same way (CMD_POWER_EVENT).
 *
 * No screen blocks this thread: screens react to keys and ticks (see
 * ui/screen.h), so the idle timeout, HOME long-press and this event queue
 * keep running whatever is on screen.
 */

#include "akira_os_shell.h"
#include "display_ownership.h"
#include "home_screen.h"
#include "shell_theme.h"
#include "wait_screen.h"
#include "ui/akira_ui.h"
#include "ui/overlay.h"
#include "ui/screen.h"

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
#if defined(CONFIG_AKIRA_USB_MSC)
#include <storage/usb_msc.h>
#endif
#if defined(CONFIG_AKIRA_FUSB302_VBUS)
#include <drivers/usbc/fusb302_vbus.h>
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
#include <lib/mem_helper.h>
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
    CMD_USB_MSC_EVENT,     /* USB MSC took/released the SD card */
    CMD_USB_TRUST_EVENT,   /* VBUS attached/removed while USB mode is IDLE */
    CMD_POWER_EVENT,       /* battery monitor: plug/unplug/full/low/level */
    CMD_LOCK,              /* enter the wait screen now */
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
        /* CMD_USB_MSC_EVENT */
        struct
        {
            bool active;
        } msc;
        /* CMD_USB_TRUST_EVENT */
        struct
        {
            bool arm; /* true = VBUS attached (arm prompt), false = removed (cancel) */
        } trust;
        /* CMD_POWER_EVENT */
        struct
        {
            uint32_t events; /* AKIRA_BATT_EVT_* */
        } power;
    };
} shell_event_t;

/* ------------------------------------------------------------------ */
/* Message queues                                                      */
/* ------------------------------------------------------------------ */

#define SHELL_EVENT_QUEUE_DEPTH 8

/* Ring buffer in PSRAM — shell_event_t is plain data and every producer
 * goes through k_msgq_put(..., K_NO_WAIT). */
AKIRA_BULK_MSGQ_DEFINE(g_shell_msgq,
                       sizeof(shell_event_t),
                       SHELL_EVENT_QUEUE_DEPTH,
                       4);

/* ------------------------------------------------------------------ */
/* Shell state                                                         */
/* ------------------------------------------------------------------ */

static bool g_wasm_active; /* true while a WASM app has the display */
#if defined(CONFIG_AKIRA_USB_MSC)
/* Set by shell_usb_event_cb() (USB stack thread) on VBUS attach while IDLE,
 * cleared by VBUS removal or once the shell thread has shown the prompt.
 * Deferred to the shell thread rather than acted on directly — the confirm
 * dialog and akira_usb_msc_enter() need display/input ownership, which only
 * the shell thread has, and must wait for HOME/idle if the user is mid-app. */
static bool g_msc_trust_pending;
#if defined(CONFIG_AKIRA_FUSB302_VBUS)
static bool usb_trust_prompt_should_cancel(void);
#endif
#endif

/* SD insert toast: shown while the SD manager registers the card's apps,
 * dismissed by the CMD_APP_STATE_CHANGED that follows (or its own timeout). */
#define SD_TOAST_LOADING_MS 6000
#define SD_TOAST_REMOVED_MS 2000
static int g_sd_toast = -1;

#if defined(CONFIG_AKIRA_USB_MSC)
/* USB MSC lock screen — a full-screen takeover like the wait screen, not a
 * small popup: while the host owns the SD card the rest of the shell is
 * locked out.  SCR_F_STICKY keeps HOME long-press from popping it. */
static void msc_lock_draw(void)
{
    int cy = SCR_H / 2 - 40;
    int w = SCR_W - 80, x = 40;

    akira_ui_dither_card(x, cy - 16, w, 108, 14, false, 5);
    /* USB trident, small */
    int ux = SCR_W / 2;
    akira_display_vline(ux, cy - 6, 22, C_WHITE);
    akira_display_triangle_fill(ux - 4, cy - 2, ux + 4, cy - 2, ux, cy - 8, C_WHITE);
    akira_display_line(ux, cy + 10, ux - 7, cy + 4, C_WHITE);
    akira_display_line(ux, cy + 14, ux + 7, cy + 8, C_WHITE);
    akira_display_circle_fill(ux, cy + 17, 3, C_WHITE);

    const char *l1 = "USB storage", *l2 = "SD card shared with host", *l3 = "Eject on the host first";
    akira_display_text((SCR_W - akira_ui_text_w(l1, AKIRA_UI_FONT_SMALL)) / 2, cy + 30, l1, C_WHITE);
    akira_display_text((SCR_W - akira_ui_text_w(l2, AKIRA_UI_FONT_SMALL)) / 2, cy + 50, l2, C_GRAY);
    akira_display_text((SCR_W - akira_ui_text_w(l3, AKIRA_UI_FONT_SMALL)) / 2, cy + 66, l3, C_GRAY);
}

static const shell_screen_t msc_lock_screen = {
    .name = "msc_lock",
    .flags = SCR_F_STATUSBAR | SCR_F_STICKY,
    .title = "USB storage",
    .draw = msc_lock_draw,
};
#endif

/* ------------------------------------------------------------------ */
/* IPC lifecycle listener thread                                       */
/* ------------------------------------------------------------------ */

/* 2048, not 1024: this thread holds a CONFIG_AKIRA_IPC_MSG_MAX_SIZE (256 B)
 * receive buffer as a stack local, on top of the akira_ipc_recv() call depth
 * and Xtensa's windowed-ABI register spills.  `kernel thread list` measured it
 * at 1024/1024 with zero unused bytes — no headroom for an interrupt taken at
 * peak depth, and the overflow lands in whatever stack the linker placed next. */
#define LIFECYCLE_LISTENER_STACK 2048
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

void akira_os_shell_notify_usb_msc(bool active)
{
    shell_event_t ev = {.type = CMD_USB_MSC_EVENT, .msc = {.active = active}};
    k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
}

void akira_os_shell_lock(void)
{
    shell_event_t ev = {.type = CMD_LOCK};
    k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
}

void akira_shell_set_wasm_launching(void)
{
    akira_display_clear(C_BLACK);
    akira_display_flush();
    g_wasm_active = true;
    screen_set_suspended(true);
    overlay_clear(); /* nothing transient should greet the user on return */
    /* Hand the app an empty edge queue: the launcher's own keystrokes (the A
     * press that started it, its release) are not input for the app. */
    akira_input_flush();
    akira_display_release_to_wasm();
}

void akira_shell_abort_wasm_launch(void)
{
    g_wasm_active = false;
    akira_display_claim_shell();
    screen_set_suspended(false);
}

/* ------------------------------------------------------------------ */
/* Shell main thread                                                   */
/* ------------------------------------------------------------------ */

/* akiraplay_screen.c runs TLS (catalog_https_get for the catalogue fetch and
 * app download) directly on this thread (via screen_run_busy()), which overflows a stack sized only
 * for UI rendering and file I/O. Zephyr's interactive console
 * (CONFIG_SHELL_STACK_SIZE) needed the same 3072->8192 bump for the same
 * reason: mbedTLS's handshake and close_notify are stack-hungry with
 * software ECDHE (P256M off). */
#define SHELL_THREAD_STACK_SIZE 8192
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
           || bt_manager_keeps_awake()                   /* host connected or scan/spam running */
#endif
#if defined(CONFIG_AKIRA_WIFI_MANAGER)
           || wifi_manager_get_state() != WIFI_MGR_STATE_IDLE
#endif
#if defined(CONFIG_AKIRA_OTA) && defined(CONFIG_FLASH_MAP) && \
    defined(CONFIG_BOOTLOADER_MCUBOOT)
           || ota_is_update_in_progress()
#endif
           || !screen_is_root()                          /* user is mid-task in a sub-screen */
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

#if defined(CONFIG_AKIRA_USB_MSC)
static void usb_trust_result(bool trusted, void *ctx)
{
    ARG_UNUSED(ctx);
    if (!trusted)
    {
        return;
    }
    int ret = akira_usb_msc_enter();
    if (ret != 0)
    {
        LOG_WRN("USB MSC enter after trust prompt failed: %d", ret);
        overlay_toast(TOAST_ICON_ERROR, "Couldn't share SD card", NULL, 2500);
    }
    /* success: akira_usb_msc_enter() already queued CMD_USB_MSC_EVENT,
     * which pushes the lock screen on the next drain. */
}
#endif

#ifdef CONFIG_AKIRA_POWER_MANAGER
/* Battery monitor events (CMD_POWER_EVENT) → what the user sees.  The status
 * bar and the wait screen follow the cached snapshot on their own; this only
 * adds the moments worth interrupting for. */
static void shell_power_event(uint32_t events, bool blanked)
{
    akira_battery_snapshot_t b;
    char line[32];

    if (g_wasm_active)
    {
        return; /* the app owns the screen; the status bar catches up later */
    }
    if (blanked)
    {
        /* Light the dimmed clock up for a moment so a plug-in (or a dying
         * battery) is acknowledged; other changes repaint quietly. */
        if (events & (AKIRA_BATT_EVT_PLUGGED | AKIRA_BATT_EVT_CRITICAL))
        {
            wait_screen_peek();
        }
        return;
    }

    akira_pm_battery_snapshot(&b);
    if (events & AKIRA_BATT_EVT_PLUGGED)
    {
        overlay_charging_splash();
    }
    if (events & AKIRA_BATT_EVT_UNPLUGGED)
    {
        snprintf(line, sizeof(line), "%u%% remaining", b.pct);
        overlay_toast(TOAST_ICON_BATTERY, "On battery", line, 2200);
    }
    if ((events & AKIRA_BATT_EVT_FULL) && !(events & AKIRA_BATT_EVT_PLUGGED))
    {
        overlay_toast(TOAST_ICON_BATTERY, "Fully charged", "Safe to unplug", 3000);
    }
    if (events & AKIRA_BATT_EVT_CRITICAL)
    {
        snprintf(line, sizeof(line), "%u%% - plug in now", b.pct);
        overlay_toast(TOAST_ICON_BATTERY, "Battery critical", line, 5000);
    }
    else if (events & AKIRA_BATT_EVT_LOW)
    {
        snprintf(line, sizeof(line), "%u%% remaining", b.pct);
        overlay_toast(TOAST_ICON_BATTERY, "Battery low", line, 3500);
    }
}
#endif

/* ---- backlight policy --------------------------------------------- */

/* Pre-blank dim level and the paused-game dim level (hardware 0..255). */
#define DIM_IDLE_HW 77 /* 30 % */
#define DIM_APP_HW  26 /* 10 % */

/* The user's backlight level from Settings → Display (stored in percent). */
static uint8_t g_user_bright = 255;

static uint8_t shell_read_user_brightness(void)
{
#ifdef CONFIG_AKIRA_SETTINGS
    char sv[16] = "";

    if (!akira_settings_get("akira/display/brightness", sv, sizeof(sv)))
    {
        int pct = CLAMP(atoi(sv), 1, 100);
        return (uint8_t)((pct * 255 + 50) / 100);
    }
#endif
    return 255;
}

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
    }
    else
    {
        LOG_WRN("Display device not ready");
    }
#endif

    /* HOME launcher at the root of the screen stack; first frame below. */
    screen_init(&home_screen);
    screen_status_tick();
    screen_frame();

    /* The display HAL lights the panel at a neutral level before settings
     * exist; switch to the user's own level as soon as they do. */
    g_user_bright = shell_read_user_brightness();
    akira_display_hal_set_brightness(g_user_bright);

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
    bool s_lock_requested = false; /* Settings → Lock screen (CMD_LOCK) */
    bool s_dimmed = false;         /* pre-blank dim is showing          */
    bool s_app_dimmed = false;     /* paused WASM app dimmed            */
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
            if (s_dimmed || s_app_dimmed)
            {
                /* Any key while dimmed: back to the user's level at once. */
                s_dimmed = false;
                s_app_dimmed = false;
                akira_display_hal_set_brightness(g_user_bright);
            }
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
                screen_set_suspended(false);
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
            if (s_app_dimmed)
            {
                /* App exited while dimmed: the shell's screens get full light. */
                s_app_dimmed = false;
                akira_display_hal_set_brightness(g_user_bright);
            }

            /* Nothing consumes the edge queue while the shell owns input —
             * the screens all work off just_pressed, derived from the bitmask.
             * Drop whatever accumulated, or the 16-slot queue saturates after
             * a handful of presses, every further edge logs an eviction
             * warning, and the next app started inherits the backlog. */
            akira_input_flush();

            /* s_prev_btns and just_pressed already computed at loop top.
             * Guard with !s_display_blanked: keys must not reach the screens
             * while the wait screen is up (it has its own HOME-hold wake). */
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
                        LOG_DBG("BTN: %s", btn_names[_b]);
                    }
                }
                screen_dispatch_key(just_pressed);
            }

            /* One frame: the top screen and overlays tick, and the framework
             * repaints only what changed (status-bar-only when just the clock
             * or the charging animation moved).  While blanked the wait screen
             * owns the panel and decides for itself whether to repaint. */
            if (!s_display_blanked)
            {
                screen_frame();
            }
            else
            {
                wait_screen_update();
            }

            /* Status strip / wait screen updated every 1 s */
            static int64_t s_last_status_ms;
            if (now_ms - s_last_status_ms >= 1000)
            {
                s_last_status_ms = now_ms;
                if (!s_display_blanked)
                {
                    screen_status_tick();
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
                    g_user_bright = shell_read_user_brightness();
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
                        screen_set_suspended(false);
                        s_prev_btns = akira_input_get_bitmask();
                    }
                }

                /* Pre-blank dim: a cheap "about to sleep" cue that also saves
                 * the last stretch of full backlight before the clock. */
#if CONFIG_AKIRA_IDLE_DIM_S > 0
                if (!s_display_blanked && s_display_timeout_ms > 0)
                {
                    int64_t dim_at = MAX(s_display_timeout_ms -
                                             (int64_t)CONFIG_AKIRA_IDLE_DIM_S * 1000,
                                         s_display_timeout_ms / 2);
                    bool due = (now_ms - s_last_input_ms) >= dim_at;

                    if (due && !s_dimmed)
                    {
                        s_dimmed = true;
                        akira_display_hal_set_brightness(MIN(g_user_bright, DIM_IDLE_HW));
                    }
                    else if (!due && s_dimmed)
                    {
                        /* Activity without a key (a host session) */
                        s_dimmed = false;
                        akira_display_hal_set_brightness(g_user_bright);
                    }
                }
#endif

                /* Idle wait-screen check */
                static int64_t s_deep_sleep_arm_ms;
                if (!s_display_blanked &&
                    (s_lock_requested ||
                     (s_display_timeout_ms > 0 &&
                      (now_ms - s_last_input_ms) >= s_display_timeout_ms)))
                {
                    s_lock_requested = false;
                    s_dimmed = false;
                    s_display_blanked = true;
                    s_deep_sleep_arm_ms = now_ms;
                    /* The user walked away: close whatever was open (sticky
                     * screens like the USB-storage lock stay) so an abandoned
                     * settings page can't hold the device out of deep sleep. */
                    screen_pop_to_root();
                    screen_set_suspended(true);
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
             * HOME down falls through to the 33 ms poll so the
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
                /* Awake tick (~30 Hz).  Button input is interrupt-driven (see
                 * akira_input_api.c), so this rate only sets animation
                 * smoothness and the granularity of the HOME hold detector.
                 * A frame with nothing to repaint costs no SPI traffic. */
                k_sleep(K_MSEC(33));
            }
        }
        else
        {
            /* Shell dormant while WASM holds display.
             * s_prev_btns is already updated at the top of the loop.
             * The app keeps the screen, but a game left paused shouldn't
             * burn full backlight: dim after the display timeout with no
             * key; any key (top of the loop) restores it. */
            if (!s_app_dimmed && s_display_timeout_ms > 0 &&
                (now_ms - s_last_input_ms) >= s_display_timeout_ms)
            {
                s_app_dimmed = true;
                akira_display_hal_set_brightness(MIN(g_user_bright, DIM_APP_HW));
            }
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

                /* Reclaim display immediately so the home screen is visible
                 * before app_manager_stop() blocks for the abort timeout. */
                g_wasm_active = false;
                akira_display_claim_shell();
                s_prev_btns = akira_input_get_bitmask();
                screen_pop_to_root();
                screen_set_suspended(s_display_blanked);

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
                        screen_set_suspended(true);
                        overlay_clear();
                        akira_input_flush();
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
                        screen_set_suspended(s_display_blanked);
                        home_screen_refresh();
                        LOG_INF("Display reclaimed by shell (all apps stopped)");
                    }
                    else if (!g_wasm_active)
                    {
                        /* Registry changed under us — most often the SD
                         * manager finishing a freshly inserted card. */
                        if (g_sd_toast >= 0)
                        {
                            overlay_toast_dismiss(g_sd_toast);
                            g_sd_toast = -1;
                        }
                        home_screen_refresh();
                    }
                }
                break;

            case CMD_INSTALL_PROGRESS:
                if (!g_wasm_active)
                {
                    const char *m = ev.install.msg;
                    bool failed = strstr(m, "fail") || strstr(m, "Fail") ||
                                  strstr(m, "error") || strstr(m, "Error");

                    overlay_progress(ev.install.name, ev.install.pct, m);
                    if (failed)
                    {
                        overlay_progress_finish(false, m);
                    }
                    else if (ev.install.pct >= 100)
                    {
                        overlay_progress_finish(true, m[0] ? m : "Installed");
                        home_screen_refresh();
                    }
                }
                break;

#ifdef CONFIG_AKIRA_SD_HOTPLUG
            case CMD_SD_CARD_EVENT:
                if (!g_wasm_active)
                {
                    if (g_sd_toast >= 0)
                    {
                        overlay_toast_dismiss(g_sd_toast);
                    }
                    if (ev.sd.present)
                    {
                        g_sd_toast = overlay_toast(TOAST_ICON_SD, "SD card inserted",
                                                   "Loading apps", SD_TOAST_LOADING_MS);
                        overlay_toast_set_busy(g_sd_toast, true);
                    }
                    else
                    {
                        g_sd_toast = -1;
                        overlay_toast(TOAST_ICON_SD, "SD card removed", NULL,
                                      SD_TOAST_REMOVED_MS);
                        home_screen_refresh();
                    }
                }
                break;
#endif

#if defined(CONFIG_AKIRA_USB_MSC)
            case CMD_USB_MSC_EVENT:
                if (ev.msc.active)
                {
                    if (!screen_is_open(&msc_lock_screen))
                    {
                        screen_push(&msc_lock_screen, NULL);
                    }
                }
                else
                {
                    screen_close(&msc_lock_screen);
                    home_screen_refresh();
                }
                break;

            case CMD_USB_TRUST_EVENT:
                g_msc_trust_pending = ev.trust.arm;
                if (!ev.trust.arm && akira_usb_msc_owns_sd())
                {
                    /* Cable pulled while MSC is mounted — don't strand the
                     * user on the lock screen with the SD card stuck
                     * unavailable; hand it back safely. Runs on this
                     * thread (ample stack, same as the confirm-then-enter
                     * path), not the FUSB302 poller's workqueue. */
                    akira_usb_msc_exit();
                }
                break;
#endif

#ifdef CONFIG_AKIRA_POWER_MANAGER
            case CMD_POWER_EVENT:
                shell_power_event(ev.power.events, s_display_blanked);
                break;
#endif

            case CMD_LOCK:
                /* Taken by the idle check in the 1 s block above. */
                s_lock_requested = true;
                break;

            default:
                break;
            }
        }

#if defined(CONFIG_AKIRA_USB_MSC)
        /* Show the deferred USB trust prompt once we're actually sitting
         * idle at HOME — never interrupts a running app, a sub-screen, another
         * dialog, or a blanked screen.  The answer arrives in
         * usb_trust_result() once the dialog has closed. */
        if (g_msc_trust_pending && !g_wasm_active && !s_display_blanked &&
            screen_is_root() && usb_manager_get_mode() != USB_MODE_MSC)
        {
            g_msc_trust_pending = false;
            akira_ui_confirm_open("USB", "Share SD card with host?",
#if defined(CONFIG_AKIRA_FUSB302_VBUS)
                                  usb_trust_prompt_should_cancel,
#else
                                  NULL,
#endif
                                  usb_trust_result, NULL);
        }
#endif
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

#if defined(CONFIG_AKIRA_USB_MSC)
static void shell_usb_msc_cb(bool owns_sd, void *user_data)
{
    ARG_UNUSED(user_data);
    akira_os_shell_notify_usb_msc(owns_sd);
}

/* VBUS attach/removal while USB mode is IDLE — fires on the USB stack
 * thread, so just arm/disarm the trust prompt; shown by the shell thread
 * once it's next at HOME (see the trust-pending check in the main loop). */
static void shell_usb_event_cb(usb_manager_event_t event, void *user_data)
{
    ARG_UNUSED(user_data);
    if (event == USB_EVENT_VBUS_ATTACH_IDLE)
    {
#ifdef CONFIG_AKIRA_POWER_MANAGER
        /* Same cable usually means a charger: re-read the battery now so the
         * charging animation starts with the plug, not on the next poll. */
        akira_pm_battery_poll_now();
#endif
        shell_event_t ev = {.type = CMD_USB_TRUST_EVENT, .trust = {.arm = true}};
        k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
    }
    else if (event == USB_EVENT_DISCONNECTED)
    {
        shell_event_t ev = {.type = CMD_USB_TRUST_EVENT, .trust = {.arm = false}};
        k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
    }
}

#if defined(CONFIG_AKIRA_FUSB302_VBUS)
/* Polled by the trust dialog every frame: answer "cancel" the moment the
 * cable goes, straight from the FUSB302's live reading rather than waiting
 * for the queued CMD_USB_TRUST_EVENT. */
static bool usb_trust_prompt_should_cancel(void)
{
    return !akira_fusb302_vbus_present();
}
#endif
#endif

#ifdef CONFIG_AKIRA_POWER_MANAGER
/* Battery monitor listener — runs on the system workqueue. */
static void shell_battery_cb(uint32_t events, const akira_battery_snapshot_t *snap,
                             void *user)
{
    ARG_UNUSED(snap);
    ARG_UNUSED(user);
    shell_event_t ev = {.type = CMD_POWER_EVENT, .power = {.events = events}};
    k_msgq_put(&g_shell_msgq, &ev, K_NO_WAIT);
    k_sem_give(&home_wake_sem); /* a blanked shell loop is parked on this */
}
#endif

static int shell_init(void)
{
#ifdef CONFIG_AKIRA_SD_HOTPLUG
    akira_sd_card_register_pre_insert_cb(shell_sd_pre_insert_cb, NULL);
    akira_sd_card_register_hotplug_cb(shell_sd_hotplug_cb, NULL);
#endif
#if defined(CONFIG_AKIRA_USB_MSC)
    akira_usb_msc_register_state_cb(shell_usb_msc_cb, NULL);
    usb_manager_register_callback(shell_usb_event_cb, NULL);
#endif

#ifdef CONFIG_AKIRA_POWER_MANAGER
    akira_pm_battery_register_cb(shell_battery_cb, NULL);
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
