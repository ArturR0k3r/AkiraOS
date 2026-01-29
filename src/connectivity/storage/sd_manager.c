/**
 * @file sd_manager.c
 * @brief SD Card Manager Implementation
 */

#include "sd_manager.h"
#include <runtime/app_manager/app_manager.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>
#include <string.h>

LOG_MODULE_REGISTER(sd_manager, CONFIG_AKIRA_LOG_LEVEL);

#define SD_DISK_NAME "SD"
#define APP_NAME_MAX 32

static sd_state_t g_state = SD_STATE_UNMOUNTED;
static sd_event_cb_t g_event_cb = NULL;
static void *g_event_user = NULL;
static K_MUTEX_DEFINE(g_sd_mutex);

/* FAT FS mount structure */
static FATFS g_fat_fs;
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

int sd_manager_init(void)
{
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
    uint32_t status;
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

    if (g_state != SD_STATE_MOUNTED)
    {
        LOG_WRN("SD card not mounted");
        return -ENODEV;
    }

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

        /* Check for .wasm extension */
        size_t len = strlen(entry.name);
        if (len > 5 && strcmp(&entry.name[len - 5], ".wasm") == 0)
        {
            /* Extract name without extension */
            size_t name_len = len - 5;
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
        return -ENODEV;
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
