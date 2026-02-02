/**
 * @file buf_pool.h
 * @brief Shared Buffer Pool for Connectivity Layer
 *
 * Provides a unified buffer pool using Zephyr's NET_BUF subsystem.
 * Replaces scattered per-module buffers with a shared 12KB pool
 * (8 x 1536B buffers) for improved memory efficiency.
 *
 * Usage:
 *   struct net_buf *buf = akira_buf_alloc(K_MSEC(100));
 *   if (buf) {
 *       // Use net_buf_add_mem(), net_buf_pull(), etc.
 *       akira_buf_unref(buf);
 *   }
 */

#ifndef AKIRA_BUF_POOL_H
#define AKIRA_BUF_POOL_H

#include <zephyr/kernel.h>
#include <zephyr/net/buf.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Buffer pool configuration
 *
 * 8 buffers x 1536 bytes = 12KB total
 * 1536B chosen for: Ethernet MTU (1500) + headers + alignment
 */
#define AKIRA_BUF_POOL_COUNT    8
#define AKIRA_BUF_SIZE          1536

/**
 * @brief Allocate a buffer from the shared pool
 *
 * @param timeout Maximum time to wait for a buffer
 * @return Pointer to net_buf, or NULL if allocation failed
 */
struct net_buf *akira_buf_alloc(k_timeout_t timeout);

/**
 * @brief Release a buffer back to the pool
 *
 * @param buf Buffer to release (NULL-safe)
 */
void akira_buf_unref(struct net_buf *buf);

/**
 * @brief Get current pool statistics
 *
 * @param free_count Output: number of free buffers
 * @param total_count Output: total buffer count
 */
void akira_buf_pool_stats(uint8_t *free_count, uint8_t *total_count);

/**
 * @brief Reset buffer for reuse (clears data, keeps allocation)
 *
 * @param buf Buffer to reset
 */
static inline void akira_buf_reset(struct net_buf *buf)
{
    if (buf) {
        net_buf_reset(buf);
    }
}

/**
 * @brief Get remaining space in buffer
 *
 * @param buf Buffer to check
 * @return Available bytes
 */
static inline size_t akira_buf_tailroom(struct net_buf *buf)
{
    return buf ? net_buf_tailroom(buf) : 0;
}

/**
 * @brief Add data to buffer tail
 *
 * @param buf Target buffer
 * @param data Data to add
 * @param len Length of data
 * @return Pointer to added data, or NULL if insufficient space
 */
static inline void *akira_buf_add(struct net_buf *buf, const void *data, size_t len)
{
    if (!buf || net_buf_tailroom(buf) < len) {
        return NULL;
    }
    return net_buf_add_mem(buf, data, len);
}

/**
 * @brief Get pointer to buffer data
 *
 * @param buf Buffer
 * @return Pointer to data start
 */
static inline uint8_t *akira_buf_data(struct net_buf *buf)
{
    return buf ? buf->data : NULL;
}

/**
 * @brief Get current data length
 *
 * @param buf Buffer
 * @return Length of data in buffer
 */
static inline size_t akira_buf_len(struct net_buf *buf)
{
    return buf ? buf->len : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BUF_POOL_H */
