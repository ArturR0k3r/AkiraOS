/**
 * @file buf_pool.h
 * @brief Shared Buffer Pool for Connectivity Layer
 *
 * Provides a simple buffer pool without Zephyr NET_BUF dependency.
 * Configuration: 8 buffers x 1536 bytes = 12KB total pool size.
 */

#ifndef AKIRA_BUF_POOL_H
#define AKIRA_BUF_POOL_H

#include <zephyr/kernel.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pool configuration */
#define AKIRA_BUF_POOL_COUNT  8
#define AKIRA_BUF_SIZE        1536

/**
 * @brief Simple buffer structure
 */
struct akira_buf {
    uint8_t data[AKIRA_BUF_SIZE];
    uint16_t len;
    uint8_t in_use;
    uint8_t reserved;
};

struct akira_buf *akira_buf_alloc(k_timeout_t timeout);
void akira_buf_unref(struct akira_buf *buf);
void akira_buf_pool_stats(uint8_t *free_count, uint8_t *total_count);

static inline void akira_buf_reset(struct akira_buf *buf)
{
    if (buf) { buf->len = 0; }
}

static inline size_t akira_buf_tailroom(struct akira_buf *buf)
{
    return buf ? (AKIRA_BUF_SIZE - buf->len) : 0;
}

static inline void akira_buf_add_len(struct akira_buf *buf, size_t len)
{
    if (buf && buf->len + len <= AKIRA_BUF_SIZE) { buf->len += len; }
}

static inline void *akira_buf_add_mem(struct akira_buf *buf, const void *mem, size_t len)
{
    if (!buf || buf->len + len > AKIRA_BUF_SIZE) { return NULL; }
    void *ptr = &buf->data[buf->len];
    if (mem) { memcpy(ptr, mem, len); }
    buf->len += len;
    return ptr;
}

static inline uint8_t *akira_buf_data(struct akira_buf *buf)
{
    return buf ? buf->data : NULL;
}

static inline size_t akira_buf_len(struct akira_buf *buf)
{
    return buf ? buf->len : 0;
}

static inline uint8_t *akira_buf_tail(struct akira_buf *buf)
{
    return buf ? &buf->data[buf->len] : NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BUF_POOL_H */
