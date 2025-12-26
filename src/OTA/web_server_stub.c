/**
 * @file web_server_stub.c
 * @brief Stub implementations of web_server API when embedded web UI is disabled
 */

#include "web_server.h"
#include <errno.h>
#include <string.h>
#include <zephyr/logging/log.h>

/* Use Kconfig-provided log level constant to avoid relying on AKIRA_LOG_LEVEL macro
 * which may not be defined if akira.h isn't included. */
LOG_MODULE_REGISTER(web_server_stub, CONFIG_AKIRA_LOG_LEVEL);

int web_server_start(const struct web_server_callbacks *callbacks)
{
    LOG_INF("Embedded web server disabled by configuration");
    ARG_UNUSED(callbacks);
    return -ENOTSUP;
}

int web_server_stop(void)
{
    return -ENOTSUP;
}

int web_server_get_stats(struct web_server_stats *stats)
{
    if (stats)
    {
        memset(stats, 0, sizeof(*stats));
        stats->state = WEB_SERVER_STOPPED;
    }
    return 0;
}

bool web_server_is_running(void)
{
    return false;
}

enum web_server_state web_server_get_state(void)
{
    return WEB_SERVER_STOPPED;
}

void web_server_notify_network_status(bool connected, const char *ip_address)
{
    ARG_UNUSED(connected);
    ARG_UNUSED(ip_address);
}

void web_server_add_log(const char *log_line)
{
    ARG_UNUSED(log_line);
}

void web_server_broadcast_log(const char *message, size_t length)
{
    ARG_UNUSED(message);
    ARG_UNUSED(length);
}

void web_server_refresh_data(void)
{
}

void web_server_set_custom_headers(const char *headers)
{
    ARG_UNUSED(headers);
}
