/**
 * @file hid_shell.c
 * @brief Shell commands for HID testing
 */

#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

#include "connectivity/hid/hid_manager.h"
#include "connectivity/bluetooth/bt_hid.h"

LOG_MODULE_REGISTER(hid_shell, CONFIG_AKIRA_LOG_LEVEL);

/* Forward declarations for sim helpers (not public header) */
#if IS_ENABLED(CONFIG_AKIRA_HID_SIM)
int hid_sim_init(void);
void hid_sim_connect(void);
void hid_sim_disconnect(void);
#endif

static int cmd_hid_enable(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argv);
    if (hid_manager_enable() == 0)
    {
        shell_print(sh, "HID enabled");
    }
    else
    {
        shell_warn(sh, "Failed to enable HID");
    }
    return 0;
}

static int cmd_hid_disable(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argv);
    if (hid_manager_disable() == 0)
    {
        shell_print(sh, "HID disabled");
    }
    else
    {
        shell_warn(sh, "Failed to disable HID");
    }
    return 0;
}

static int cmd_hid_transport(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_print(sh, "Usage: hid transport <ble|sim>");
        return 0;
    }

    if (strcmp(argv[1], "ble") == 0)
    {
        if (hid_manager_set_transport(HID_TRANSPORT_BLE) == 0)
            shell_print(sh, "Transport: BLE");
        else
            shell_warn(sh, "Failed to set transport");
    }
    else if (strcmp(argv[1], "sim") == 0)
    {
        if (hid_manager_set_transport(HID_TRANSPORT_SIMULATED) == 0)
            shell_print(sh, "Transport: SIM");
        else
            shell_warn(sh, "Failed to set transport");
    }
    else if (strcmp(argv[1], "usb") == 0)
    {
        if (hid_manager_set_transport(HID_TRANSPORT_USB) == 0)
            shell_print(sh, "Transport: USB");
        else
            shell_warn(sh, "Failed to set transport");
    }
    else
    {
        shell_print(sh, "Unknown transport: %s", argv[1]);
    }
    return 0;
}

static int cmd_kb_type(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_print(sh, "Usage: hid keyboard type <string>");
        return 0;
    }

    /* Combine remaining args into a single string */
    size_t total = 0;
    for (size_t i = 1; i < argc; i++)
        total += strlen(argv[i]) + 1;

    char buf[128] = {0};
    size_t pos = 0;
    for (size_t i = 1; i < argc; i++)
    {
        if (pos + strlen(argv[i]) + 1 >= sizeof(buf))
            break;
        memcpy(buf + pos, argv[i], strlen(argv[i]));
        pos += strlen(argv[i]);
        if (i + 1 < argc)
        {
            buf[pos++] = ' ';
        }
    }

    if (hid_keyboard_type_string(buf) == 0)
    {
        shell_print(sh, "Typed: %s", buf);
    }
    else
    {
        shell_warn(sh, "Failed to send string");
    }

    return 0;
}

static int cmd_sim(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_print(sh, "Usage: hid sim <connect|disconnect>");
        return 0;
    }

    if (strcmp(argv[1], "connect") == 0)
    {
        hid_sim_connect();
        shell_print(sh, "HID sim: connected");
    }
    else if (strcmp(argv[1], "disconnect") == 0)
    {
        hid_sim_disconnect();
        shell_print(sh, "HID sim: disconnected");
    }
    else
    {
        shell_print(sh, "Unknown sim command: %s", argv[1]);
    }
    return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
    const hid_state_t *s = hid_manager_get_state();
    if (!s)
    {
        shell_print(sh, "HID: not initialized");
        return 0;
    }
    shell_print(sh, "HID state: enabled=%d connected=%d transport=%s reports_sent=%u errors=%u",
                s->enabled, s->connected, transport_to_string(s->transport), s->reports_sent, s->errors);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(hid_keyboard_commands,
                               SHELL_CMD_ARG(type, NULL, "Type a string via HID keyboard", cmd_kb_type, 2, 255),
                               SHELL_SUBCMD_SET_END);

#if IS_ENABLED(CONFIG_AKIRA_HID_SIM)
SHELL_STATIC_SUBCMD_SET_CREATE(hid_sim_commands,
                               SHELL_CMD_ARG(connect, NULL, "Connect HID sim", cmd_sim, 2, 0),
                               SHELL_CMD_ARG(disconnect, NULL, "Disconnect HID sim", cmd_sim, 2, 0),
                               SHELL_SUBCMD_SET_END);
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(hid_commands,
                               SHELL_CMD_ARG(enable, NULL, "Enable HID subsystem", cmd_hid_enable, 1, 0),
                               SHELL_CMD_ARG(disable, NULL, "Disable HID subsystem", cmd_hid_disable, 1, 0),
                               SHELL_CMD_ARG(transport, NULL, "Set HID transport (ble|sim)", cmd_hid_transport, 2, 0),
                               SHELL_CMD(status, NULL, "HID status", cmd_status),
#if IS_ENABLED(CONFIG_AKIRA_HID_SIM)
                               SHELL_CMD(sim, &hid_sim_commands, "Simulation transport commands", NULL),
#endif
                               SHELL_CMD(keyboard, &hid_keyboard_commands, "Keyboard commands", NULL),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(hid, &hid_commands, "HID commands", NULL);
