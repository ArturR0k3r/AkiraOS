/**
 * @file usb_storage.c
 * @brief USB Mass Storage Manager Implementation
 *
 * Mounts a FAT filesystem on a USB flash drive connected while AkiraOS acts
 * as USB host (subsys/usb/host/class/usbh_msc.c does the enumeration/SCSI
 * work and registers a disk_access disk; this file just reacts to it).
 * With CONFIG_USBH_MASS_STORAGE_CLASS off, behaves as a permanent no-op
 * stub so builds without USB host support are unaffected.
 */

#include "usb_storage.h"
#include <runtime/app_manager/app_manager.h>
#include <lib/akpkg.h>
#include <lib/mem_helper.h>
#include <storage/fs_manager.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>

#if defined(CONFIG_USBH_MASS_STORAGE_CLASS)
#include <zephyr/usb/class/usbh_msc.h>
#include <ff.h>
#endif

LOG_MODULE_REGISTER(usb_storage, CONFIG_AKIRA_LOG_LEVEL);

#define APP_NAME_MAX 32

static usb_storage_state_t g_state = USB_STORAGE_DISCONNECTED;
static usb_storage_event_cb_t g_event_cb = NULL;
static void *g_event_user = NULL;

static void notify_state_change(usb_storage_state_t new_state)
{
    if (g_state != new_state)
    {
        g_state = new_state;
        if (g_event_cb)
        {
            g_event_cb(new_state, g_event_user);
        }
    }
}

#if defined(CONFIG_USBH_MASS_STORAGE_CLASS)

static FATFS g_usb_fat_fs;
static struct fs_mount_t g_usb_mount = {
    .type = FS_FATFS,
    .fs_data = &g_usb_fat_fs,
    .mnt_point = USB_MOUNT_POINT,
};

static void usb_msc_event(bool connected, void *user_data)
{
    ARG_UNUSED(user_data);

    if (connected)
    {
        notify_state_change(USB_STORAGE_CONNECTED);

        int ret = fs_mount(&g_usb_mount);
        if (ret < 0)
        {
            LOG_ERR("Failed to mount USB storage: %d", ret);
            notify_state_change(USB_STORAGE_ERROR);
            return;
        }

        LOG_INF("USB storage mounted at %s", USB_MOUNT_POINT);
        notify_state_change(USB_STORAGE_MOUNTED);
    }
    else
    {
        if (g_state == USB_STORAGE_MOUNTED)
        {
            int ret = fs_unmount(&g_usb_mount);
            if (ret < 0)
            {
                LOG_ERR("Failed to unmount USB storage: %d", ret);
            }
        }
        notify_state_change(USB_STORAGE_DISCONNECTED);
    }
}

int usb_storage_init(void)
{
    usbh_msc_register_callback(usb_msc_event, NULL);
    LOG_INF("USB Storage Manager initialized (host MSC enabled)");
    return 0;
}

#else /* !CONFIG_USBH_MASS_STORAGE_CLASS */

int usb_storage_init(void)
{
    LOG_INF("USB Storage Manager initialized (host MSC not built in)");
    return 0;
}

#endif

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
    if (!names || max_count <= 0)
    {
        return -EINVAL;
    }

    if (g_state != USB_STORAGE_MOUNTED)
    {
        LOG_WRN("USB storage not mounted");
        return -ENODEV;
    }

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);

    int ret = fs_opendir(&dir, USB_APPS_DIR);
    if (ret < 0)
    {
        LOG_ERR("Failed to open %s: %d", USB_APPS_DIR, ret);
        return ret;
    }

    int count = 0;
    struct fs_dirent entry;

    while (count < max_count && fs_readdir(&dir, &entry) == 0)
    {
        if (entry.name[0] == '\0')
        {
            break;
        }

        /* Accept .wasm and .akpkg extensions */
        size_t len = strlen(entry.name);
        size_t name_len = 0;
        if (len > 5 && strcmp(&entry.name[len - 5], ".wasm") == 0)
        {
            name_len = len - 5;
        }
        else if (len > 6 && strcmp(&entry.name[len - 6], ".akpkg") == 0)
        {
            name_len = len - 6;
        }
        if (name_len > 0)
        {
            if (name_len >= APP_NAME_MAX)
            {
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
    if (!name)
    {
        return -EINVAL;
    }

    if (g_state != USB_STORAGE_MOUNTED)
    {
        return -ENODEV;
    }

    char path[64];

    /* Try .akpkg first, then fall back to .wasm */
    snprintf(path, sizeof(path), "%s/%s.akpkg", USB_APPS_DIR, name);
    if (fs_manager_exists(path))
    {
        ssize_t size = fs_manager_get_size(path);
        if (size > 0)
        {
            uint8_t *buf = akira_malloc_buffer((size_t)size);
            if (!buf)
            {
                return -ENOMEM;
            }
            ssize_t rd = fs_manager_read_file(path, buf, (size_t)size);
            int ret = -EIO;
            if (rd == size)
            {
                char name_buf[APP_NAME_MAX_LEN];
                strncpy(name_buf, name, sizeof(name_buf) - 1);
                name_buf[sizeof(name_buf) - 1] = '\0';
                ret = app_manager_install_akpkg(name_buf, sizeof(name_buf),
                                                buf, (size_t)size,
                                                APP_SOURCE_USB);
            }
            akira_free_buffer(buf);
            return ret;
        }
    }

    snprintf(path, sizeof(path), "%s/%s.wasm", USB_APPS_DIR, name);
    return app_manager_install_from_path(path);
}

void usb_storage_register_callback(usb_storage_event_cb_t callback, void *user_data)
{
    g_event_cb = callback;
    g_event_user = user_data;
}
