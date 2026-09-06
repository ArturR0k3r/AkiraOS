/**
 * @file sd_manager.c
 * @brief SD Card Manager Implementation
 */

#include "sd_manager.h"
#include <runtime/app_manager/app_manager.h>
#include <console_shell/akira_os_shell.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>
#include <string.h>
#if defined(CONFIG_AKIRA_SD_CARD)
#include <storage/sd_card.h>
#include <storage/fs_manager.h>
#endif
#if defined(CONFIG_AKIRA_USB_MSC)
#include <storage/usb_msc.h>
#endif
#include "lib/mem_helper.h"

LOG_MODULE_REGISTER(sd_manager, CONFIG_AKIRA_LOG_LEVEL);

#define SD_DISK_NAME "SD"
#define APP_NAME_MAX 32

static sd_state_t g_state = SD_STATE_UNMOUNTED;
static sd_event_cb_t g_event_cb = NULL;
static void *g_event_user = NULL;
static K_MUTEX_DEFINE(g_sd_mutex);

/* FAT FS mount structure */
static FATFS g_fat_fs AKIRA_BULK_BSS;
static struct fs_mount_t g_mount = {
    .type = FS_FATFS,
    .fs_data = &g_fat_fs,
    .mnt_point = SD_MOUNT_POINT,
};

static void notify_state_change(sd_state_t new_state)
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

#ifdef CONFIG_AKIRA_SD_HOTPLUG
static void sd_manager_hotplug_cb(bool present, void *user_data)
{
    ARG_UNUSED(user_data);
    if (present) {
        fs_manager_reinit_sd();
        notify_state_change(SD_STATE_MOUNTED);
        app_manager_register_sd_apps();
    } else {
        app_manager_unregister_sd_apps();
        notify_state_change(SD_STATE_UNMOUNTED);
    }
    akira_os_shell_notify_app_changed();
}
#endif

#if defined(CONFIG_AKIRA_USB_MSC)
/* MSC took/released the SD card — the registry and screen never hear about
 * this otherwise, since it isn't a physical hotplug event. */
static void sd_manager_msc_cb(bool owns_sd, void *user_data)
{
    ARG_UNUSED(user_data);
    if (owns_sd) {
        app_manager_unregister_sd_apps();
        notify_state_change(SD_STATE_UNMOUNTED);
    } else {
        app_manager_register_sd_apps();
        notify_state_change(SD_STATE_MOUNTED);
    }
    akira_os_shell_notify_app_changed();
}
#endif

static void sd_state_log_cb(sd_state_t state, void *user_data)
{
    ARG_UNUSED(user_data);
    static const char *const names[] = {"UNMOUNTED", "MOUNTED", "ERROR"};
    LOG_INF("SD state → %s", names[state]);
}

int sd_manager_init(void)
{
#ifdef CONFIG_AKIRA_SD_HOTPLUG
    akira_sd_card_register_hotplug_cb(sd_manager_hotplug_cb, NULL); /* slot 0 */
#endif
#if defined(CONFIG_AKIRA_USB_MSC)
    akira_usb_msc_register_state_cb(sd_manager_msc_cb, NULL);
#endif
    sd_manager_register_callback(sd_state_log_cb, NULL);

    /* If card was present at boot, register apps now */
    if (akira_sd_card_is_present()) {
        LOG_INF("SD card detected at boot");
        notify_state_change(SD_STATE_MOUNTED);
        app_manager_register_sd_apps();
    }

    LOG_INF("SD Manager initialized");
    return 0;
}

int sd_manager_mount(void)
{
    k_mutex_lock(&g_sd_mutex, K_FOREVER);

    if (g_state == SD_STATE_MOUNTED)
    {
        k_mutex_unlock(&g_sd_mutex);
        return 0;
    }

#if defined(CONFIG_AKIRA_SD_CARD)
    /* sd_card.c already mounted the filesystem at /SD: via SYS_INIT.
     * If the card was absent at boot (hot-plug), attempt a re-probe now. */
    if (!akira_sd_card_is_present())
    {
        int ret = akira_sd_card_init();
        if (ret < 0)
        {
            LOG_ERR("SD card not present: %d", ret);
            notify_state_change(SD_STATE_ERROR);
            k_mutex_unlock(&g_sd_mutex);
            return -ENODEV;
        }
        LOG_INF("SD card detected after hot-plug");
        /* Update fs_manager's availability flag so SD paths are recognized */
        fs_manager_reinit_sd();
    }
    notify_state_change(SD_STATE_MOUNTED);
    LOG_INF("SD card available at %s (via sd_card driver)", SD_MOUNT_POINT);
    k_mutex_unlock(&g_sd_mutex);
    return 0;
#else
    /* Check if disk is available */
    int ret = disk_access_init(SD_DISK_NAME);
    if (ret < 0)
    {
        LOG_ERR("SD card not detected: %d", ret);
        notify_state_change(SD_STATE_ERROR);
        k_mutex_unlock(&g_sd_mutex);
        return ret;
    }

    /* Get disk status */
    ret = disk_access_status(SD_DISK_NAME);
    if (ret != DISK_STATUS_OK)
    {
        LOG_ERR("SD card status error: %d", ret);
        notify_state_change(SD_STATE_ERROR);
        k_mutex_unlock(&g_sd_mutex);
        return -EIO;
    }

    /* Mount filesystem */
    ret = fs_mount(&g_mount);
    if (ret < 0)
    {
        LOG_ERR("Failed to mount SD card: %d", ret);
        notify_state_change(SD_STATE_ERROR);
        k_mutex_unlock(&g_sd_mutex);
        return ret;
    }

    notify_state_change(SD_STATE_MOUNTED);
    LOG_INF("SD card mounted at %s", SD_MOUNT_POINT);
    k_mutex_unlock(&g_sd_mutex);
    return 0;
#endif
}

int sd_manager_unmount(void)
{
    k_mutex_lock(&g_sd_mutex, K_FOREVER);

    if (g_state != SD_STATE_MOUNTED)
    {
        k_mutex_unlock(&g_sd_mutex);
        return 0;
    }

    int ret = fs_unmount(&g_mount);
    if (ret < 0)
    {
        LOG_ERR("Failed to unmount SD card: %d", ret);
        k_mutex_unlock(&g_sd_mutex);
        return ret;
    }

    notify_state_change(SD_STATE_UNMOUNTED);
    LOG_INF("SD card unmounted");

    k_mutex_unlock(&g_sd_mutex);
    return 0;
}

bool sd_manager_is_mounted(void)
{
    return g_state == SD_STATE_MOUNTED;
}

sd_state_t sd_manager_get_state(void)
{
    return g_state;
}

int sd_manager_scan_apps(char names[][32], int max_count)
{
    if (!names || max_count <= 0)
    {
        return -EINVAL;
    }

    /* Auto-mount if not already done */
    if (g_state != SD_STATE_MOUNTED)
    {
        int ret = sd_manager_mount();
        if (ret < 0)
        {
            LOG_WRN("SD card not available: %d", ret);
            return ret;
        }
    }

    /* Decompress any .akpkg archives into plain .wasm/.aot first. */
    app_manager_extract_sd_akpkgs(SD_APPS_DIR);

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);

    int ret = fs_opendir(&dir, SD_APPS_DIR);
    if (ret < 0)
    {
        /* Try to create apps directory */
        ret = fs_mkdir(SD_APPS_DIR);
        if (ret < 0 && ret != -EEXIST)
        {
            LOG_ERR("Failed to access %s: %d", SD_APPS_DIR, ret);
            return ret;
        }
        /* Try again */
        ret = fs_opendir(&dir, SD_APPS_DIR);
        if (ret < 0)
        {
            return ret;
        }
    }

    int count = 0;
    struct fs_dirent entry;

    while (count < max_count && fs_readdir(&dir, &entry) == 0)
    {
        if (entry.name[0] == '\0')
        {
            break;
        }

        /* Check for .wasm/.WASM or .aot/.AOT extension (case-insensitive) */
        size_t len = strlen(entry.name);
        bool is_wasm = len > 5 &&
            entry.name[len - 5] == '.' &&
            ((entry.name[len - 4] | 0x20) == 'w') &&
            ((entry.name[len - 3] | 0x20) == 'a') &&
            ((entry.name[len - 2] | 0x20) == 's') &&
            ((entry.name[len - 1] | 0x20) == 'm');
        bool is_aot = len > 4 &&
            entry.name[len - 4] == '.' &&
            ((entry.name[len - 3] | 0x20) == 'a') &&
            ((entry.name[len - 2] | 0x20) == 'o') &&
            ((entry.name[len - 1] | 0x20) == 't');
        int ext_len = is_wasm ? 5 : (is_aot ? 4 : 0);
        if (ext_len > 0)
        {
            /* Extract name without extension */
            size_t name_len = len - (size_t)ext_len;
            if (name_len >= APP_NAME_MAX)
            {
                name_len = APP_NAME_MAX - 1;
            }
            strncpy(names[count], entry.name, name_len);
            names[count][name_len] = '\0';

            LOG_DBG("Found app: %s (%zu bytes)", names[count], entry.size);
            count++;
        }
    }

    fs_closedir(&dir);
    LOG_INF("Found %d apps in %s", count, SD_APPS_DIR);
    return count;
}

int sd_manager_install_app(const char *name)
{
    if (!name)
    {
        return -EINVAL;
    }

    if (g_state != SD_STATE_MOUNTED)
    {
        int ret = sd_manager_mount();
        if (ret < 0)
        {
            return ret;
        }
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.wasm", SD_APPS_DIR, name);

    return app_manager_install_from_path(path);
}

int sd_manager_install_all_apps(void)
{
    char names[8][32];
    int count = sd_manager_scan_apps(names, 8);

    if (count <= 0)
    {
        return count;
    }

    int installed = 0;
    for (int i = 0; i < count; i++)
    {
        int ret = sd_manager_install_app(names[i]);
        if (ret >= 0)
        {
            installed++;
            LOG_INF("Installed app from SD: %s", names[i]);
        }
        else
        {
            LOG_WRN("Failed to install %s: %d", names[i], ret);
        }
    }

    return installed;
}

void sd_manager_register_callback(sd_event_cb_t callback, void *user_data)
{
    g_event_cb = callback;
    g_event_user = user_data;
}
