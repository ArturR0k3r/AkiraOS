/*
 * stub_settings.c — in-memory akira_settings for AkiraOS tests.
 *
 * settings.c needs the flash/SD storage backends; the suites that only need
 * get/set/delete (the boot guard's trial record) link this map instead.
 */

#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>
#include "settings/settings.h"

#define STUB_SETTINGS_SLOTS 8

static struct {
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
    bool used;
} stub_settings[STUB_SETTINGS_SLOTS];

static int find(const char *key)
{
    for (int i = 0; i < STUB_SETTINGS_SLOTS; i++) {
        if (stub_settings[i].used && strcmp(stub_settings[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

int akira_settings_get(const char *key, char *value, size_t max_len)
{
    int i = find(key);

    if (i < 0) {
        return -ENOENT;
    }
    if (strlen(stub_settings[i].value) >= max_len) {
        return -E2BIG;
    }
    strcpy(value, stub_settings[i].value);
    return 0;
}

int akira_settings_set(const char *key, const char *value, uint8_t is_encrypted)
{
    (void)is_encrypted;
    int i = find(key);

    if (strlen(key) >= MAX_KEY_LEN || strlen(value) >= MAX_VALUE_LEN) {
        return -E2BIG;
    }
    for (int j = 0; i < 0 && j < STUB_SETTINGS_SLOTS; j++) {
        if (!stub_settings[j].used) {
            i = j;
        }
    }
    if (i < 0) {
        return -ENOMEM;
    }
    strcpy(stub_settings[i].key, key);
    strcpy(stub_settings[i].value, value);
    stub_settings[i].used = true;
    return 0;
}

int akira_settings_delete(const char *key)
{
    int i = find(key);

    if (i < 0) {
        return -ENOENT;
    }
    stub_settings[i].used = false;
    return 0;
}
