/**
 * @file buf_pool.c
 * @brief Shared Buffer Pool Implementation
 *
 * Implements a unified buffer pool using Zephyr's NET_BUF subsystem.
 * Pool size: 8 buffers x 1536 bytes = 12KB total
 *
 * This replaces scattered per-module buffers (HTTP, OTA, etc.) with
 * a shared pool for better memory utilization and reduced fragmentation.
 */

#include "buf_pool.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(buf_pool, CONFIG_AKIRA_LOG_LEVEL);

/*
 * Define the shared buffer pool using Zephyr's NET_BUF_POOL_DEFINE
 *
 * Parameters:
 * - akira_buf_pool: pool name
 * - AKIRA_BUF_POOL_COUNT: number of buffers (8)
 * - AKIRA_BUF_SIZE: size of each buffer (1536 bytes)
 * - 4: user data size (unused, set to minimum)
 * - NULL: no destroy callback
 */
NET_BUF_POOL_DEFINE(akira_buf_pool, AKIRA_BUF_POOL_COUNT, AKIRA_BUF_SIZE, 4, NULL);

/* Pool usage tracking for statistics */
static uint8_t g_allocated_count = 0;
static K_MUTEX_DEFINE(pool_mutex);

struct net_buf *akira_buf_alloc(k_timeout_t timeout)
{
    struct net_buf *buf = net_buf_alloc(&akira_buf_pool, timeout);

    if (buf) {
        k_mutex_lock(&pool_mutex, K_FOREVER);
        g_allocated_count++;
        k_mutex_unlock(&pool_mutex);

        LOG_DBG("Buffer allocated (%u/%u in use)", g_allocated_count, AKIRA_BUF_POOL_COUNT);
    } else {
        LOG_WRN("Buffer allocation failed (pool exhausted)");
    }

    return buf;
}

void akira_buf_unref(struct net_buf *buf)
{
    if (!buf) {
        return;
    }

    k_mutex_lock(&pool_mutex, K_FOREVER);
    if (g_allocated_count > 0) {
        g_allocated_count--;
    }
    k_mutex_unlock(&pool_mutex);

    LOG_DBG("Buffer released (%u/%u in use)", g_allocated_count, AKIRA_BUF_POOL_COUNT);

    net_buf_unref(buf);
}

void akira_buf_pool_stats(uint8_t *free_count, uint8_t *total_count)
{
    k_mutex_lock(&pool_mutex, K_FOREVER);

    if (free_count) {
        *free_count = AKIRA_BUF_POOL_COUNT - g_allocated_count;
    }
    if (total_count) {
        *total_count = AKIRA_BUF_POOL_COUNT;
    }

    k_mutex_unlock(&pool_mutex);
}
