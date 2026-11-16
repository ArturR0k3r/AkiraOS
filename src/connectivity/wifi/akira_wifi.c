/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public Wi-Fi façade (include/akira_wifi.h) over wifi_manager.
 */

#include <akira_wifi.h>
#include "wifi_manager.h"

#include <errno.h>
#include <string.h>
#include <zephyr/sys/util.h>

int akira_wifi_connect(void)
{
	return wifi_manager_connect();
}

int akira_wifi_disconnect(void)
{
	return wifi_manager_disconnect();
}

int akira_wifi_set_credentials(const char *ssid, const char *psk)
{
	return wifi_manager_update_credentials(ssid, psk);
}

int akira_wifi_scan(void)
{
	return wifi_manager_scan();
}

int akira_wifi_scan_results(struct akira_wifi_network *out, size_t max)
{
	wifi_mgr_scan_result_t r[8];
	size_t count = 0;

	if (!out || max == 0) {
		return -EINVAL;
	}
	int ret = wifi_manager_get_scan_results(r, MIN(max, ARRAY_SIZE(r)), &count);

	if (ret < 0) {
		return ret;
	}
	for (size_t i = 0; i < count; i++) {
		memcpy(out[i].ssid, r[i].ssid, sizeof(out[i].ssid));
		out[i].ssid[sizeof(out[i].ssid) - 1] = '\0';
		out[i].rssi = r[i].rssi;
		out[i].security = r[i].security;
		out[i].channel = r[i].channel;
	}
	return (int)count;
}

int akira_wifi_get_status(struct akira_wifi_status *out)
{
	wifi_mgr_stats_t st;

	if (!out) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));
	int ret = wifi_manager_get_stats(&st);

	if (ret < 0) {
		return ret;
	}
	out->connected = st.ip_addr[0] != '\0';
	out->rssi = st.rssi;
	out->channel = st.channel;
	strncpy(out->ssid, st.ssid, sizeof(out->ssid) - 1);
	strncpy(out->ip_addr, st.ip_addr, sizeof(out->ip_addr) - 1);
	return 0;
}

int akira_wifi_ap_start(const char *ssid, const char *psk)
{
	return wifi_manager_ap_start(ssid, psk);
}

int akira_wifi_ap_stop(void)
{
	return wifi_manager_ap_stop();
}
