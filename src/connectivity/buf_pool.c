/**
 * @file buf_pool.c
 * @brief Shared Buffer Pool Implementation
 *
 * Simple buffer pool: 8 buffers x 1536 bytes = 12KB total
 */

#include "buf_pool.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(buf_pool, CONFIG_AKIRA_LOG_LEVEL);

static struct akira_buf g_buf_pool[AKIRA_BUF_POOL_COUNT] __aligned(4);
static K_MUTEX_DEFINE(pool_mutex);
static K_SEM_DEFINE(pool_sem, AKIRA_BUF_POOL_COUNT, AKIRA_BUF_POOL_COUNT);

struct akira_buf *akira_buf_alloc(k_timeout_t timeout)
{
    if (k_sem_take(&pool_sem, timeout) != 0) {
        LOG_WRN("Buffer pool exhausted");
        return NULL;
    }

    k_mutex_lock(&pool_mutex, K_FOREVER);
    for (int i = 0; i < AKIRA_BUF_POOL_COUNT; i++) {
        if (!g_buf_pool[i].in_use) {
            g_buf_pool[i].in_use = 1;
            g_buf_pool[i].len = 0;
            k_mutex_unlock(&pool_mutex);
            LOG_DBG("Buffer %d allocated", i);
            return &g_buf_pool[i];
        }
    }
    k_mutex_unlock(&pool_mutex);
    k_sem_give(&pool_sem);
    return NULL;
}

void akira_buf_unref(struct akira_buf *buf)
{
    if (!buf) { return; }

    k_mutex_lock(&pool_mutex, K_FOREVER);
    if (buf->in_use) {
        buf->in_use = 0;
        buf->len = 0;
        k_sem_give(&pool_sem);
    }
    k_mutex_unlock(&pool_mutex);
}

void akira_buf_pool_stats(uint8_t *free_count, uint8_t *total_count)
{
    if (total_count) { *total_count = AKIRA_BUF_POOL_COUNT; }
    if (free_count) { *free_count = (uint8_t)k_sem_count_get(&pool_sem); }
}
