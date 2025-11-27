/**
 * @file akira_system_api.c
 * @brief System API implementation for WASM exports
 */

#include "akira_api.h"
#include "../drivers/akira_hal.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_system_api, LOG_LEVEL_INF);

// TODO: Add capability check for sensitive operations
// TODO: Add per-container resource tracking

uint64_t akira_system_uptime_ms(void)
{
	return k_uptime_get();
}

size_t akira_system_free_memory(void)
{
	// TODO: Get actual heap stats
#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
	struct sys_memory_stats stats;
	sys_heap_runtime_stats_get(NULL, &stats);
	return stats.free_bytes;
#else
	return 0;
#endif
}

const char *akira_system_platform(void)
{
	return akira_get_platform_name();
}

void akira_system_sleep(uint32_t ms)
{
	// TODO: Limit max sleep time for container
	// TODO: Update container CPU usage accounting
	k_msleep(ms);
}

void akira_log(int level, const char *message)
{
	// TODO: Prefix with container name
	// TODO: Rate limit logging
	
	if (!message) {
		return;
	}
	
	switch (level) {
	case 0:
		LOG_ERR("[APP] %s", message);
		break;
	case 1:
		LOG_WRN("[APP] %s", message);
		break;
	case 2:
		LOG_INF("[APP] %s", message);
		break;
	case 3:
	default:
		LOG_DBG("[APP] %s", message);
		break;
	}
}
