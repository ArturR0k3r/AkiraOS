/**
 * @file thread_manager.h
 * @brief OpenThread Network Management for AkiraOS
 *
 * Provides Thread mesh networking support with hardware-agnostic radio binding.
 * Implements Thread network formation, joining, and border router functionality.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
*/

#ifndef AKIRA_THREAD_MANAGER_H
#define AKIRA_THREAD_MANAGER_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thread device role */
typedef enum {
    THREAD_ROLE_DISABLED = 0,    /* Thread disabled */
    THREAD_ROLE_DETACHED,        /* Not attached to network */
    THREAD_ROLE_CHILD,           /* Child (end device) */
    THREAD_ROLE_ROUTER,          /* Router */
    THREAD_ROLE_LEADER,          /* Leader */
} thread_role_t;

/* Thread network state */
typedef enum {
    THREAD_STATE_DISABLED = 0,
    THREAD_STATE_DETACHED,
    THREAD_STATE_CHILD,
    THREAD_STATE_ROUTER,
    THREAD_STATE_LEADER,
} thread_state_t;

/* Thread configuration */
typedef struct {
    char network_name[17];       /* Network name (max 16 chars) */
    uint16_t panid;              /* PAN ID */
    uint8_t channel;             /* Channel (11-26) */
    uint8_t extended_panid[8];   /* Extended PAN ID */
    uint8_t network_key[16];     /* Network master key */
    bool is_ftd;                 /* Full Thread Device (vs Minimal) */
    bool enable_border_router;   /* Enable border router */
} thread_config_t;

/* Thread statistics */
typedef struct {
    thread_role_t role;
    uint16_t rloc16;            /* Router locator */
    uint8_t leader_router_id;   /* Leader router ID */
    uint8_t partition_id;       /* Partition ID */
    uint32_t child_count;       /* Number of children (if router/leader) */
    uint32_t neighbor_count;    /* Number of neighbors */
    uint64_t packets_sent;
    uint64_t packets_received;
    int8_t rssi;
} thread_stats_t;

/**
 * @brief Initialize Thread manager
 *
 * @param config Thread configuration
 * @return 0 on success, negative errno on failure
 */
int thread_manager_init(const thread_config_t *config);

/**
 * @brief Start Thread network
 *
 * Starts Thread interface and begins network formation or joining.
 *
 * @return 0 on success, negative errno on failure
 */
int thread_start(void);

/**
 * @brief Stop Thread network
 *
 * @return 0 on success, negative errno on failure
 */
int thread_stop(void);

/**
 * @brief Get Thread statistics
 *
 * @param stats Pointer to statistics structure
 * @return 0 on success, negative errno on failure
 */
int thread_get_stats(thread_stats_t *stats);

/**
 * @brief Get Thread network dataset
 *
 * @param buffer Buffer to store dataset
 * @param buffer_len Buffer length
 * @return Number of bytes written, or negative errno on failure
 */
int thread_get_dataset(uint8_t *buffer, size_t buffer_len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_THREAD_MANAGER_H */
