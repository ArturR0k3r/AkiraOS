/**
 * @file ha_discovery.c
 * @brief Home Assistant MQTT-Discovery helpers (light entity).
 */

#include "ha_discovery.h"
#include "mqtt_service.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(ha_discovery, CONFIG_AKIRA_LOG_LEVEL);

/* ------------------------------------------------------------------------- */
/* Pure JSON helpers                                                         */
/* ------------------------------------------------------------------------- */

int ha_build_light_config(char *buf, size_t cap, const char *node,
                          const char *object, const char *name)
{
    if (!buf || !node || !object || !name) {
        return -EINVAL;
    }
    int n = snprintf(buf, cap,
        "{\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"schema\":\"json\","
        "\"command_topic\":\"%s/light/%s/set\","
        "\"state_topic\":\"%s/light/%s/state\","
        "\"brightness\":true,"
        "\"supported_color_modes\":[\"rgb\"],"
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"AkiraOS %s\","
        "\"manufacturer\":\"PenEngineering\",\"model\":\"AkiraOS\"}}",
        name, node, object, node, object, node, object, node, node);
    if (n < 0 || (size_t)n >= cap) {
        return -ENOMEM;
    }
    return n;
}

int ha_build_light_state(char *buf, size_t cap, int on, int brightness,
                         int r, int g, int b)
{
    if (!buf) {
        return -EINVAL;
    }
    int n;
    if (on) {
        n = snprintf(buf, cap,
            "{\"state\":\"ON\",\"brightness\":%d,\"color_mode\":\"rgb\","
            "\"color\":{\"r\":%d,\"g\":%d,\"b\":%d}}",
            brightness & 0xFF, r & 0xFF, g & 0xFF, b & 0xFF);
    } else {
        n = snprintf(buf, cap, "{\"state\":\"OFF\"}");
    }
    if (n < 0 || (size_t)n >= cap) {
        return -ENOMEM;
    }
    return n;
}

/* Find `"key"`, skip to ':' then to the first digit, return it via atoi.
 * @return true if the key/number was found. */
static bool json_find_int(const char *json, const char *key, int *out)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    /* skip to ':' */
    while (*p && *p != ':') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p && (*p == ' ' || *p == '"')) {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    *out = atoi(p);
    return true;
}

int ha_parse_light_command(const char *json, size_t len, int *on,
                           int *brightness, int *r, int *g, int *b)
{
    if (!json || len == 0) {
        return 0;
    }
    int fields = 0;

    /* state: look for the value after "state". */
    const char *st = strstr(json, "\"state\"");
    if (st) {
        /* ON appears in the value; OFF is the off case. strstr for "OFF"
         * first because "ON" is a substring of neither key nor "OFF". */
        if (strstr(st, "OFF")) {
            if (on) { *on = 0; }
            fields |= HA_CMD_STATE;
        } else if (strstr(st, "ON")) {
            if (on) { *on = 1; }
            fields |= HA_CMD_STATE;
        }
    }

    int v;
    if (json_find_int(json, "brightness", &v)) {
        if (brightness) { *brightness = v; }
        fields |= HA_CMD_BRIGHTNESS;
    }

    /* color object: {"color":{"r":..,"g":..,"b":..}} */
    const char *col = strstr(json, "\"color\"");
    if (col) {
        int rr = 0, gg = 0, bb = 0;
        bool ok = json_find_int(col, "r", &rr) &&
                  json_find_int(col, "g", &gg) &&
                  json_find_int(col, "b", &bb);
        if (ok) {
            if (r) { *r = rr; }
            if (g) { *g = gg; }
            if (b) { *b = bb; }
            fields |= HA_CMD_COLOR;
        }
    }
    return fields;
}

/* ------------------------------------------------------------------------- */
/* High-level API                                                            */
/* ------------------------------------------------------------------------- */

int ha_light_register(const char *object_id, const char *name)
{
    if (!object_id || !name) {
        return -EINVAL;
    }
    const char *node = mqtt_service_node_id();
    char config_topic[AKIRA_MQTT_TOPIC_MAX];
    char command_topic[AKIRA_MQTT_TOPIC_MAX];
    char config[384];   /* discovery config JSON (~280 chars) */

    snprintf(config_topic, sizeof(config_topic),
             "homeassistant/light/%s_%s/config", node, object_id);
    snprintf(command_topic, sizeof(command_topic),
             "%s/light/%s/set", node, object_id);

    int n = ha_build_light_config(config, sizeof(config), node, object_id, name);
    if (n < 0) {
        LOG_ERR("discovery config too large for %s", object_id);
        return n;
    }

    int rc = mqtt_service_publish(config_topic, config, n, 1, true);
    if (rc != 0) {
        return rc;
    }
    return mqtt_service_subscribe(command_topic);
}

int ha_light_report(const char *object_id, int on, int brightness,
                    int r, int g, int b)
{
    if (!object_id) {
        return -EINVAL;
    }
    const char *node = mqtt_service_node_id();
    char state_topic[AKIRA_MQTT_TOPIC_MAX];
    char state[AKIRA_MQTT_PAYLOAD_MAX];

    snprintf(state_topic, sizeof(state_topic),
             "%s/light/%s/state", node, object_id);
    int n = ha_build_light_state(state, sizeof(state), on, brightness, r, g, b);
    if (n < 0) {
        return n;
    }
    return mqtt_service_publish(state_topic, state, n, 0, true);
}

int ha_light_poll(const char *object_id, int *on, int *brightness,
                  int *r, int *g, int *b, int timeout_ms)
{
    if (!object_id) {
        return -EINVAL;
    }
    const char *node = mqtt_service_node_id();
    char want[AKIRA_MQTT_TOPIC_MAX];
    snprintf(want, sizeof(want), "%s/light/%s/set", node, object_id);

    int64_t deadline = k_uptime_get() + timeout_ms;
    do {
        int64_t remaining = deadline - k_uptime_get();
        if (remaining < 0) {
            remaining = 0;
        }
        struct akira_mqtt_message msg;
        int rc = mqtt_service_poll_message(&msg, K_MSEC(remaining));
        if (rc != 0) {
            return 0;   /* timeout, no matching command */
        }
        if (strcmp(msg.topic, want) == 0) {
            return ha_parse_light_command((const char *)msg.payload,
                                          msg.payload_len, on, brightness,
                                          r, g, b);
        }
        /* not ours — keep draining until the deadline */
    } while (k_uptime_get() < deadline);

    return 0;
}
