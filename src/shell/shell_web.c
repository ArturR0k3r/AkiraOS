/**
 * @file shell_web.c
 * @brief Web server shell commands
 *
 * Separated from akira_shell.c to allow conditional compilation
 * based on CONFIG_AKIRA_HTTP_SERVER
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/logging/log.h>
#include "../connectivity/ota/web_server.h"

LOG_MODULE_REGISTER(shell_web, CONFIG_LOG_DEFAULT_LEVEL);

/* Web server status command */
static int cmd_web_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    enum web_server_state state = web_server_get_state();
    const char *state_str;

    switch (state)
    {
    case WEB_SERVER_STOPPED:
        state_str = "Stopped";
        break;
    case WEB_SERVER_STARTING:
        state_str = "Starting";
        break;
    case WEB_SERVER_RUNNING:
        state_str = "Running";
        break;
    case WEB_SERVER_ERROR:
        state_str = "Error";
        break;
    default:
        state_str = "Unknown";
    }

    shell_print(sh, "\n=== Web Server Status ===");
    shell_print(sh, "State: %s", state_str);

    /* Get IP address to show URL */
    struct net_if *iface = net_if_get_default();
    if (iface)
    {
        char addr_str[NET_IPV4_ADDR_LEN];
        struct in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
        if (addr)
        {
            net_addr_ntop(AF_INET, addr, addr_str, sizeof(addr_str));
            shell_print(sh, "URL: http://%s:%d/", addr_str, HTTP_PORT);
        }
    }

    return 0;
}

static int cmd_web_start(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    struct net_if *iface = net_if_get_default();
    if (!iface)
    {
        shell_print(sh, "No network interface");
        return -ENODEV;
    }

    char addr_str[NET_IPV4_ADDR_LEN];
    struct in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (!addr)
    {
        shell_print(sh, "No IP address - connect to WiFi first");
        return -ENOTCONN;
    }

    net_addr_ntop(AF_INET, addr, addr_str, sizeof(addr_str));
    shell_print(sh, "Starting web server at http://%s:%d/", addr_str, HTTP_PORT);

    web_server_notify_network_status(true, addr_str);

    return 0;
}

/* Register web server shell commands */
SHELL_CMD_REGISTER(web_status, NULL, "Show web server status", cmd_web_status);
SHELL_CMD_REGISTER(web_start, NULL, "Start web server", cmd_web_start);
