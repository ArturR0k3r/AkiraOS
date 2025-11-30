/**
 * @file usb_storage.c
 * @brief USB Mass Storage Manager Implementation
 *
 * Stub implementation for USB storage app discovery.
 * Full implementation requires USB host mode support.
 */

#include "usb_storage.h"
#include "../../services/app_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>

LOG_MODULE_REGISTER(usb_storage, CONFIG_AKIRA_LOG_LEVEL);

#define APP_NAME_MAX     32

static usb_storage_state_t g_state = USB_STORAGE_DISCONNECTED;
static usb_storage_event_cb_t g_event_cb = NULL;
static void *g_event_user = NULL;

int usb_storage_init(void)
{
    LOG_INF("USB Storage Manager initialized");
    /* TODO: Register USB host events when available */
    return 0;
}

bool usb_storage_is_mounted(void)
{
    return g_state == USB_STORAGE_MOUNTED;
}

usb_storage_state_t usb_storage_get_state(void)
{
    return g_state;
}

int usb_storage_scan_apps(char names[][32], int max_count)
{
    if (!names || max_count <= 0) {
        return -EINVAL;
    }

    if (g_state != USB_STORAGE_MOUNTED) {
        LOG_WRN("USB storage not mounted");
        return -ENODEV;
    }

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);

    int ret = fs_opendir(&dir, USB_APPS_DIR);
    if (ret < 0) {
        LOG_ERR("Failed to open %s: %d", USB_APPS_DIR, ret);
        return ret;
    }

    int count = 0;
    struct fs_dirent entry;

    while (count < max_count && fs_readdir(&dir, &entry) == 0) {
        if (entry.name[0] == '\0') {
            break;
        }

        /* Check for .wasm extension */
        size_t len = strlen(entry.name);
        if (len > 5 && strcmp(&entry.name[len - 5], ".wasm") == 0) {
            size_t name_len = len - 5;
            if (name_len >= APP_NAME_MAX) {
                name_len = APP_NAME_MAX - 1;
            }
            strncpy(names[count], entry.name, name_len);
            names[count][name_len] = '\0';
            count++;
        }
    }

    fs_closedir(&dir);
    LOG_INF("Found %d apps in %s", count, USB_APPS_DIR);
    return count;
}

int usb_storage_install_app(const char *name)
{
    if (!name) {
        return -EINVAL;
    }

    if (g_state != USB_STORAGE_MOUNTED) {
        return -ENODEV;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.wasm", USB_APPS_DIR, name);

    return app_manager_install_from_path(path);
}

void usb_storage_register_callback(usb_storage_event_cb_t callback, void *user_data)
{
    g_event_cb = callback;
    g_event_user = user_data;
}
