/**
 * @file shell_http.c
 * @brief HTTP server shell commands
 *
 * Replaces the deleted shell_web.c.  Uses the new http_server.c API
 * instead of the removed web_server.c API.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/logging/log.h>
#include "http_server.h"

LOG_MODULE_REGISTER(shell_http, CONFIG_LOG_DEFAULT_LEVEL);

static int cmd_http_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    http_server_state_t state = akira_http_server_get_state();
    const char *state_str;

    switch (state)
    {
    case HTTP_SERVER_STOPPED:
        state_str = "Stopped";
        break;
    case HTTP_SERVER_STARTING:
        state_str = "Starting";
        break;
    case HTTP_SERVER_RUNNING:
        state_str = "Running";
        break;
    case HTTP_SERVER_ERROR:
        state_str = "Error";
        break;
    default:
        state_str = "Unknown";
        break;
    }

    shell_print(sh, "\n=== HTTP Server Status ===");
    shell_print(sh, "State: %s", state_str);

    struct net_if *iface = net_if_get_wifi_sta();
    if (iface)
    {
        char addr_str[NET_IPV4_ADDR_LEN];
        struct in_addr *addr =
            net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
        if (addr)
        {
            net_addr_ntop(AF_INET, addr, addr_str, sizeof(addr_str));
            shell_print(sh, "URL: http://%s:%d/", addr_str,
                        HTTP_SERVER_PORT);
        }
    }

    http_server_stats_t stats;
    if (akira_http_get_stats(&stats) == 0)
    {
        shell_print(sh, "Requests: %u", stats.requests_handled);
        shell_print(sh, "Connections: %u", stats.active_connections);
    }

    return 0;
}

static int cmd_http_start(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    struct net_if *iface = net_if_get_wifi_sta();
    if (!iface)
    {
        shell_error(sh, "No network interface");
        return -ENODEV;
    }

    struct in_addr *addr =
        net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (!addr)
    {
        shell_error(sh, "No IP address - connect to WiFi first");
        return -ENOTCONN;
    }

    char addr_str[NET_IPV4_ADDR_LEN];
    net_addr_ntop(AF_INET, addr, addr_str, sizeof(addr_str));
    shell_print(sh, "HTTP server reachable at http://%s:%d/",
                addr_str, HTTP_SERVER_PORT);

    akira_http_notify_network(true, addr_str);

    return 0;
}

SHELL_CMD_REGISTER(http_status, NULL, "Show HTTP server status",
                   cmd_http_status);
SHELL_CMD_REGISTER(http_start, NULL, "Show HTTP server IP address",
                   cmd_http_start);
