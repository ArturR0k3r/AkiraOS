#include "akira_shell.h"
#include <zephyr/settings/settings.h>
#include <string.h>
#include <stdio.h>

#define DEVICE_ID_KEY "device/id"
#define WIFI_SSID_KEY "wifi/ssid"
#define WIFI_PASSCODE_KEY "wifi/passcode"
#define WIFI_ENABLED "wifi/isEnabled"

// Register 'akira' command and its `config` subcommand
void register_akira_shell(void)
{
}