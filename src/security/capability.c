/**
 * @file capability.c
 * @brief AkiraOS Capability System Implementation
 */

#include "capability.h"
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_capability, LOG_LEVEL_INF);

// TODO: Move to Kconfig
#define MAX_CONTAINERS 16

static akira_cap_set_t g_cap_sets[MAX_CONTAINERS];
static int g_cap_count = 0;

// TODO: Add mutex for thread safety
// TODO: Add persistence to NVS
// TODO: Add capability inheritance for child containers

int capability_init(void)
{
    memset(g_cap_sets, 0, sizeof(g_cap_sets));
    g_cap_count = 0;
    LOG_INF("Capability system initialized");
    return 0;
}

static akira_cap_set_t *find_cap_set(const char *name)
{
    if (!name)
    {
        return NULL;
    }

    for (int i = 0; i < g_cap_count; i++)
    {
        if (strcmp(g_cap_sets[i].container_name, name) == 0)
        {
            return &g_cap_sets[i];
        }
    }

    return NULL;
}

int capability_set(const char *name, uint32_t caps)
{
    if (!name)
    {
        return -1;
    }

    akira_cap_set_t *set = find_cap_set(name);

    if (set)
    {
        set->flags = caps;
        LOG_INF("Updated capabilities for %s: 0x%08X", name, caps);
        return 0;
    }

    if (g_cap_count >= MAX_CONTAINERS)
    {
        LOG_ERR("Max containers reached");
        return -2;
    }

    set = &g_cap_sets[g_cap_count++];
    strncpy(set->container_name, name, sizeof(set->container_name) - 1);
    set->flags = caps;

    LOG_INF("Set capabilities for %s: 0x%08X", name, caps);
    return 0;
}

bool capability_check(const char *name, akira_capability_t cap)
{
    // TODO: Add logging for denied capabilities
    // TODO: Add rate limiting for frequent checks

    akira_cap_set_t *set = find_cap_set(name);
    if (!set)
    {
        return false;
    }

    return (set->flags & cap) != 0;
}

uint32_t capability_get(const char *name)
{
    akira_cap_set_t *set = find_cap_set(name);
    if (!set)
    {
        return CAP_NONE;
    }
    return set->flags;
}

int capability_revoke(const char *name, akira_capability_t cap)
{
    akira_cap_set_t *set = find_cap_set(name);
    if (!set)
    {
        return -1;
    }

    set->flags &= ~cap;
    LOG_INF("Revoked capability 0x%08X from %s", cap, name);
    return 0;
}

// TODO: Complete capability string mapping
static const struct
{
    const char *name;
    akira_capability_t cap;
} cap_strings[] = {
    {"display.read", CAP_DISPLAY_READ},
    {"display.write", CAP_DISPLAY_WRITE},
    {"input.read", CAP_INPUT_READ},
    {"input.callback", CAP_INPUT_CALLBACK},
    {"rf.init", CAP_RF_INIT},
    {"rf.transceive", CAP_RF_TRANSCEIVE},
    {"rf.config", CAP_RF_CONFIG},
    {"sensor.imu", CAP_SENSOR_IMU},
    {"sensor.env", CAP_SENSOR_ENV},
    {"sensor.power", CAP_SENSOR_POWER},
    {"sensor.light", CAP_SENSOR_LIGHT},
    {"storage.read", CAP_STORAGE_READ},
    {"storage.write", CAP_STORAGE_WRITE},
    {"network.http", CAP_NETWORK_HTTP},
    {"network.mqtt", CAP_NETWORK_MQTT},
    {"network.raw", CAP_NETWORK_RAW},
    {"system.info", CAP_SYSTEM_INFO},
    {"system.reboot", CAP_SYSTEM_REBOOT},
    {"system.settings", CAP_SYSTEM_SETTINGS},
    {"bt.advertise", CAP_BT_ADVERTISE},
    {"bt.connect", CAP_BT_CONNECT},
    {"bt.hid", CAP_BT_HID},
    {"ipc.send", CAP_IPC_SEND},
    {"ipc.receive", CAP_IPC_RECEIVE},
    {"ipc.shm", CAP_IPC_SHM},
    {NULL, CAP_NONE}};

akira_capability_t capability_from_string(const char *cap_str)
{
    if (!cap_str)
    {
        return CAP_NONE;
    }

    for (int i = 0; cap_strings[i].name != NULL; i++)
    {
        if (strcmp(cap_strings[i].name, cap_str) == 0)
        {
            return cap_strings[i].cap;
        }
    }

    LOG_WRN("Unknown capability: %s", cap_str);
    return CAP_NONE;
}

const char *capability_to_string(akira_capability_t cap)
{
    for (int i = 0; cap_strings[i].name != NULL; i++)
    {
        if (cap_strings[i].cap == cap)
        {
            return cap_strings[i].name;
        }
    }
    return "unknown";
}
