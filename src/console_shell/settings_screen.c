/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_settings
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_shell_settings, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file settings_screen.c
 * @brief Settings menu plus the Bluetooth and About pages.
 *
 * Every page is a list_model_t rendered by the generic list screen
 * (ui/list_screen.c); Wi-Fi and Date & Time are custom screens of their own.
 */

#include "settings_screen.h"
#include "akira_os_shell.h"
#include "status_model.h"
#include "ui/akira_ui.h"
#include "ui/list_screen.h"
#include "ui/overlay.h"
#include "settings/apps_screen.h"
#include "settings/datetime_screen.h"
#include "settings/devmode_screen.h"
#include "settings/display_screen.h"
#include "settings/ota_screen.h"
#include "settings/power_screen.h"
#if defined(CONFIG_AKIRA_WIFI_MANAGER)
#include "settings/wifi_screen.h"
#endif

#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_BT
#include <connectivity/bluetooth/bt_manager.h>
#if defined(CONFIG_AKIRA_BT_COMPANION)
#include <settings/settings.h>
#include <zephyr/sys/reboot.h>
#endif
#endif

/* ---- Bluetooth page ----------------------------------------------------- */

#ifdef CONFIG_BT
static void val_bt_state(const list_item_t *it, char *buf, size_t len)
{
    static const char *const names[] = {
        [BT_STATE_OFF] = "Off",
        [BT_STATE_INITIALIZING] = "Starting",
        [BT_STATE_READY] = "Ready",
        [BT_STATE_ADVERTISING] = "Advertising",
        [BT_STATE_CONNECTED] = "Connected",
        [BT_STATE_PAIRING] = "Pairing",
        [BT_STATE_ERROR] = "Error",
    };
    bt_state_t st = bt_manager_get_state();

    ARG_UNUSED(it);
    snprintf(buf, len, "%s", (st < ARRAY_SIZE(names) && names[st]) ? names[st] : "?");
}

static void val_bt_addr(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    if (bt_manager_get_address(buf, len) != 0) {
        snprintf(buf, len, "---");
    }
}

static void act_adv_toggle(const list_item_t *it)
{
    ARG_UNUSED(it);
    if (bt_manager_get_state() == BT_STATE_ADVERTISING) {
        bt_manager_stop_advertising();
        overlay_toast(TOAST_ICON_INFO, "Bluetooth", "Stopped advertising", 1800);
    } else {
        int ret = bt_manager_start_advertising();

        overlay_toast(ret == 0 ? TOAST_ICON_OK : TOAST_ICON_ERROR, "Bluetooth",
                      ret == 0 ? "Advertising - visible to hosts" : "Couldn't advertise",
                      2000);
    }
}

static void val_adv(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "%s", bt_manager_get_state() == BT_STATE_ADVERTISING ? "On" : "Off");
}

static void unpair_done(bool confirmed, void *ctx)
{
    ARG_UNUSED(ctx);
    if (!confirmed) {
        return;
    }
    int ret = bt_manager_unpair_all();

    overlay_toast(ret == 0 ? TOAST_ICON_OK : TOAST_ICON_ERROR,
                  ret == 0 ? "Forgot all devices" : "Unpair failed", NULL, 2000);
}

static void act_unpair(const list_item_t *it)
{
    ARG_UNUSED(it);
    akira_ui_confirm_open("BT_UNPAIR", "Forget all paired devices?", NULL, unpair_done,
                          NULL);
}

#if defined(CONFIG_AKIRA_BT_COMPANION)
static const char *const g_bt_modes[] = { "HID", "Companion" };

static void reboot_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(g_reboot_work, reboot_work_fn);

static int get_bt_mode(const list_item_t *it)
{
    ARG_UNUSED(it);
    return bt_manager_boot_mode_is_companion() ? 1 : 0;
}

static void set_bt_mode(const list_item_t *it, int v)
{
    ARG_UNUSED(it);
    if (v == get_bt_mode(it)) {
        return;
    }
    /* Mode is chosen at boot (HID and Companion are mutually exclusive), so
     * persist it and restart once the toast has been seen. */
    akira_settings_set(AKIRA_BT_MODE_KEY, v ? "companion" : "hid", 0);
    overlay_toast(TOAST_ICON_INFO, v ? "BLE mode: Companion" : "BLE mode: HID",
                  "Restarting", TOAST_STICKY);
    k_work_reschedule(&g_reboot_work, K_MSEC(1500));
}
#endif

static const list_item_t g_bt_items[] = {
    { .kind = LI_INFO, .label = "State", .value = val_bt_state },
    { .kind = LI_INFO, .label = "Address", .value = val_bt_addr },
    { .kind = LI_ACTION, .label = "Advertising", .action = act_adv_toggle,
      .value = val_adv },
    { .kind = LI_ACTION, .label = "Forget all devices", .action = act_unpair },
#if defined(CONFIG_AKIRA_BT_COMPANION)
    { .kind = LI_CHOICE, .label = "BLE mode", .get = get_bt_mode, .set = set_bt_mode,
      .choices = g_bt_modes, .n_choices = ARRAY_SIZE(g_bt_modes) },
#endif
};

static const list_model_t bt_page = {
    .title = "Bluetooth",
    .items = g_bt_items,
    .count = ARRAY_SIZE(g_bt_items),
    .live = true,
};
#endif /* CONFIG_BT */

/* ---- About page --------------------------------------------------------- */

static void val_version(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "v%s", CONFIG_AKIRA_OS_VERSION);
}

static void val_board(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "%s", CONFIG_BOARD);
}

static void val_built(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "%s", __DATE__);
}

static void val_runtime(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "WAMR (WebAssembly)");
}

static void val_uptime(const list_item_t *it, char *buf, size_t len)
{
    uint32_t s = (uint32_t)(k_uptime_get() / 1000);

    ARG_UNUSED(it);
    snprintf(buf, len, "%02u:%02u:%02u", s / 3600, (s % 3600) / 60, s % 60);
}

static const list_item_t g_about_items[] = {
    { .kind = LI_INFO, .label = "AkiraOS", .value = val_version },
    { .kind = LI_INFO, .label = "Board", .value = val_board },
    { .kind = LI_INFO, .label = "Built", .value = val_built },
    { .kind = LI_INFO, .label = "App runtime", .value = val_runtime },
    { .kind = LI_INFO, .label = "Uptime", .value = val_uptime },
};

static const list_model_t about_page = {
    .title = "About",
    .items = g_about_items,
    .count = ARRAY_SIZE(g_about_items),
    .live = true,
};

/* ---- Main menu ---------------------------------------------------------- */

#if defined(CONFIG_AKIRA_WIFI_MANAGER)
static void act_wifi(const list_item_t *it)
{
    ARG_UNUSED(it);
    wifi_open();
}

static void val_wifi(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "%s", status_model_get()->wifi_on ? "Connected" : "Off");
}
#endif

static void act_datetime(const list_item_t *it)
{
    ARG_UNUSED(it);
    datetime_open();
}

static void val_datetime(const list_item_t *it, char *buf, size_t len)
{
    const shell_status_t *st = status_model_get();

    ARG_UNUSED(it);
    snprintf(buf, len, "%s", st->time_set ? st->clock : "Not set");
}

static void val_power(const list_item_t *it, char *buf, size_t len)
{
    const shell_status_t *st = status_model_get();

    ARG_UNUSED(it);
    if (st->batt_valid) {
        snprintf(buf, len, "%u%%", st->batt_pct);
    } else {
        buf[0] = '\0';
    }
}

static void act_lock(const list_item_t *it)
{
    ARG_UNUSED(it);
    akira_os_shell_lock();
}

static const list_item_t g_main_items[] = {
#if defined(CONFIG_AKIRA_WIFI_MANAGER)
    { .kind = LI_ACTION, .label = "Wi-Fi", .action = act_wifi, .value = val_wifi },
#endif
#ifdef CONFIG_BT
    { .kind = LI_NAV, .label = "Bluetooth", .screen = &list_screen, .arg = (void *)&bt_page },
#endif
    { .kind = LI_ACTION, .label = "Date & time", .action = act_datetime,
      .value = val_datetime },
    { .kind = LI_NAV, .label = "Display", .screen = &list_screen,
      .arg = (void *)&display_page },
    { .kind = LI_NAV, .label = "Power & battery", .screen = &list_screen,
      .arg = (void *)&power_page, .value = val_power },
    { .kind = LI_NAV, .label = "Apps", .screen = &list_screen, .arg = (void *)&apps_page },
    { .kind = LI_NAV, .label = "Software update", .screen = &list_screen,
      .arg = (void *)&ota_page },
    { .kind = LI_NAV, .label = "Developer", .screen = &list_screen,
      .arg = (void *)&devmode_page },
    { .kind = LI_NAV, .label = "About", .screen = &list_screen, .arg = (void *)&about_page },
    { .kind = LI_ACTION, .label = "Lock screen", .action = act_lock },
};

static const list_model_t settings_page = {
    .title = "Settings",
    .items = g_main_items,
    .count = ARRAY_SIZE(g_main_items),
    .live = true,
};

void settings_open(void)
{
    list_screen_open(&settings_page);
}
