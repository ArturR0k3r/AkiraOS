/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Zephyr-compatible shim for sys/random.h
 * 
 * This header provides getrandom() function for embedded systems
 * where the standard sys/random.h is not available.
 */

#ifndef ZEPHYR_SYS_RANDOM_H_
#define ZEPHYR_SYS_RANDOM_H_

#include <sys/types.h>
#include <zephyr/random/random.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sys/random.h getrandom flags */
#define GRND_NONBLOCK 1
#define GRND_RANDOM 2

/* Zephyr-based implementation of getrandom */
static inline ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
    if (!buf || buflen == 0)
        return -1;
    
    /* Use Zephyr's random number generator */
    size_t bytes_read = 0;
    uint8_t *p = (uint8_t *)buf;
    
    while (bytes_read < buflen) {
        uint32_t random_val = sys_rand32_get();
        size_t to_copy = (buflen - bytes_read) < sizeof(uint32_t) ? 
                         (buflen - bytes_read) : sizeof(uint32_t);
        
        memcpy(p + bytes_read, &random_val, to_copy);
        bytes_read += to_copy;
    }
    
    return (ssize_t)bytes_read;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SYS_RANDOM_H_ */
