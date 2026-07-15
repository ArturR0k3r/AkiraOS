/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the Home Assistant MQTT-Discovery JSON build/parse routines
 * (ha_discovery.c). These are pure functions, so the MQTT transport is stubbed
 * out — no broker or networking required.
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <errno.h>

#include "mqtt_service.h"
#include "ha_discovery.h"

/* ---- Transport stubs (satisfy ha_discovery.c's high-level helpers) -------- */
const char *mqtt_service_node_id(void) { return "akira_test"; }
int mqtt_service_publish(const char *topic, const void *payload, size_t len,
                         int qos, bool retain)
{
    ARG_UNUSED(topic); ARG_UNUSED(payload); ARG_UNUSED(len);
    ARG_UNUSED(qos); ARG_UNUSED(retain);
    return 0;
}
int mqtt_service_subscribe(const char *topic) { ARG_UNUSED(topic); return 0; }
int mqtt_service_poll_message(struct akira_mqtt_message *out, k_timeout_t t)
{
    ARG_UNUSED(out); ARG_UNUSED(t);
    return -EAGAIN;
}

/* ---- Discovery config JSON ----------------------------------------------- */
ZTEST(mqtt_ha, test_build_config)
{
    char buf[512];
    int n = ha_build_light_config(buf, sizeof(buf), "akira_test", "rgb", "Akira RGB");
    zassert_true(n > 0, "build returned %d", n);
    zassert_not_null(strstr(buf, "\"schema\":\"json\""), "missing schema");
    zassert_not_null(strstr(buf, "\"unique_id\":\"akira_test_rgb\""), "bad unique_id");
    zassert_not_null(strstr(buf, "\"command_topic\":\"akira_test/light/rgb/set\""), "bad cmd topic");
    zassert_not_null(strstr(buf, "\"state_topic\":\"akira_test/light/rgb/state\""), "bad state topic");
    zassert_not_null(strstr(buf, "\"supported_color_modes\":[\"rgb\"]"), "missing rgb mode");
    zassert_not_null(strstr(buf, "\"brightness\":true"), "missing brightness");
}

ZTEST(mqtt_ha, test_build_config_overflow)
{
    char small[16];
    int n = ha_build_light_config(small, sizeof(small), "akira_test", "rgb", "Akira RGB");
    zassert_equal(n, -ENOMEM, "expected overflow, got %d", n);
}

/* ---- State JSON ---------------------------------------------------------- */
ZTEST(mqtt_ha, test_build_state_on)
{
    char buf[128];
    int n = ha_build_light_state(buf, sizeof(buf), 1, 200, 255, 128, 0);
    zassert_true(n > 0, "build returned %d", n);
    zassert_not_null(strstr(buf, "\"state\":\"ON\""), "missing ON");
    zassert_not_null(strstr(buf, "\"brightness\":200"), "missing brightness");
    zassert_not_null(strstr(buf, "\"r\":255"), "missing r");
    zassert_not_null(strstr(buf, "\"g\":128"), "missing g");
    zassert_not_null(strstr(buf, "\"b\":0"), "missing b");
}

ZTEST(mqtt_ha, test_build_state_off)
{
    char buf[64];
    int n = ha_build_light_state(buf, sizeof(buf), 0, 0, 0, 0, 0);
    zassert_true(n > 0, "build returned %d", n);
    zassert_str_equal(buf, "{\"state\":\"OFF\"}", "off state wrong: %s", buf);
}

/* ---- Command parsing ----------------------------------------------------- */
ZTEST(mqtt_ha, test_parse_on_only)
{
    int on = -1, bri = -1, r = -1, g = -1, b = -1;
    const char *cmd = "{\"state\":\"ON\"}";
    int f = ha_parse_light_command(cmd, strlen(cmd), &on, &bri, &r, &g, &b);
    zassert_equal(f, HA_CMD_STATE, "fields=%d", f);
    zassert_equal(on, 1, "on=%d", on);
    zassert_equal(bri, -1, "brightness should be untouched");
}

ZTEST(mqtt_ha, test_parse_off)
{
    int on = -1, bri = 0, r = 0, g = 0, b = 0;
    const char *cmd = "{\"state\":\"OFF\"}";
    int f = ha_parse_light_command(cmd, strlen(cmd), &on, &bri, &r, &g, &b);
    zassert_true(f & HA_CMD_STATE, "no state field");
    zassert_equal(on, 0, "on=%d", on);
}

ZTEST(mqtt_ha, test_parse_brightness)
{
    int on = 0, bri = 0, r = 0, g = 0, b = 0;
    const char *cmd = "{\"state\":\"ON\",\"brightness\":128}";
    int f = ha_parse_light_command(cmd, strlen(cmd), &on, &bri, &r, &g, &b);
    zassert_true(f & HA_CMD_STATE, "no state");
    zassert_true(f & HA_CMD_BRIGHTNESS, "no brightness");
    zassert_equal(on, 1);
    zassert_equal(bri, 128, "bri=%d", bri);
}

ZTEST(mqtt_ha, test_parse_color_full)
{
    int on = 0, bri = 0, r = 0, g = 0, b = 0;
    const char *cmd =
        "{\"state\":\"ON\",\"brightness\":64,\"color\":{\"r\":10,\"g\":20,\"b\":30}}";
    int f = ha_parse_light_command(cmd, strlen(cmd), &on, &bri, &r, &g, &b);
    zassert_true(f & HA_CMD_STATE, "no state");
    zassert_true(f & HA_CMD_BRIGHTNESS, "no brightness");
    zassert_true(f & HA_CMD_COLOR, "no color");
    zassert_equal(on, 1);
    zassert_equal(bri, 64, "bri=%d", bri);
    zassert_equal(r, 10, "r=%d", r);
    zassert_equal(g, 20, "g=%d", g);
    zassert_equal(b, 30, "b=%d", b);
}

ZTEST(mqtt_ha, test_parse_empty)
{
    int on = 5, bri = 5, r = 5, g = 5, b = 5;
    int f = ha_parse_light_command("", 0, &on, &bri, &r, &g, &b);
    zassert_equal(f, 0, "expected no fields");
    zassert_equal(on, 5, "should be untouched");
}

ZTEST_SUITE(mqtt_ha, NULL, NULL, NULL, NULL, NULL);
