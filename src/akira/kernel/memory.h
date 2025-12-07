/**
 * @file memory.h
 * @brief AkiraOS Memory Management Interface
 *
 * Provides memory pool management, heap allocation tracking,
 * and memory protection for processes.
 */

#ifndef AKIRA_KERNEL_MEMORY_H
#define AKIRA_KERNEL_MEMORY_H

#include "types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*===========================================================================*/
/* Constants                                                                 */
/*===========================================================================*/

/** Maximum number of memory pools */
#define AKIRA_MAX_MEMORY_POOLS 8

/** Maximum number of memory regions per process */
#define AKIRA_MAX_MEMORY_REGIONS 16

/** Default pool block size */
#define AKIRA_POOL_DEFAULT_BLOCK 64

/*===========================================================================*/
/* Memory Protection Flags                                                   */
/*===========================================================================*/

/** Memory is readable */
#define AKIRA_MEM_READ (1 << 0)

/** Memory is writable */
#define AKIRA_MEM_WRITE (1 << 1)

/** Memory is executable */
#define AKIRA_MEM_EXEC (1 << 2)

/** Memory is cacheable */
#define AKIRA_MEM_CACHED (1 << 3)

/** Memory is for DMA */
#define AKIRA_MEM_DMA (1 << 4)

/** Memory is shared between processes */
#define AKIRA_MEM_SHARED (1 << 5)

    /*===========================================================================*/
    /* Memory Pool Types                                                         */
    /*===========================================================================*/

    /** Memory pool type */
    typedef enum
    {
        AKIRA_POOL_FIXED,    /**< Fixed-size block pool */
        AKIRA_POOL_VARIABLE, /**< Variable-size heap */
        AKIRA_POOL_SLAB      /**< Slab allocator */
    } akira_pool_type_t;

    /*===========================================================================*/
    /* Memory Structures                                                         */
    /*===========================================================================*/

    /** Memory pool configuration */
    typedef struct
    {
        const char *name;       /**< Pool name */
        akira_pool_type_t type; /**< Pool type */
        size_t total_size;      /**< Total pool size */
        size_t block_size;      /**< Block size (for fixed pools) */
        void *buffer;           /**< Pre-allocated buffer (optional) */
        uint32_t flags;         /**< Memory flags */
    } akira_pool_config_t;

    /** Memory pool handle */
    typedef struct akira_pool akira_pool_t;

    /** Memory region descriptor */
    typedef struct
    {
        void *base;        /**< Base address */
        size_t size;       /**< Region size */
        uint32_t flags;    /**< Protection flags */
        akira_pid_t owner; /**< Owning process */
    } akira_mem_region_t;

    /** Memory statistics */
    typedef struct
    {
        size_t total_bytes;      /**< Total memory managed */
        size_t used_bytes;       /**< Memory in use */
        size_t free_bytes;       /**< Available memory */
        size_t peak_usage;       /**< Peak memory usage */
        uint32_t alloc_count;    /**< Total allocations */
        uint32_t free_count;     /**< Total frees */
        uint32_t alloc_failures; /**< Allocation failures */
    } akira_mem_stats_t;

    /*===========================================================================*/
    /* Memory Pool API                                                           */
    /*===========================================================================*/

    /**
     * @brief Initialize the memory subsystem
     * @return 0 on success, negative on error
     */
    int akira_memory_init(void);

    /**
     * @brief Create a memory pool
     * @param config Pool configuration
     * @return Pool handle or NULL on error
     */
    akira_pool_t *akira_pool_create(const akira_pool_config_t *config);

    /**
     * @brief Destroy a memory pool
     * @param pool Pool to destroy
     */
    void akira_pool_destroy(akira_pool_t *pool);

    /**
     * @brief Allocate from a pool
     * @param pool Pool to allocate from
     * @param size Size to allocate (for variable pools)
     * @return Allocated memory or NULL
     */
    void *akira_pool_alloc(akira_pool_t *pool, size_t size);

    /**
     * @brief Allocate zeroed memory from a pool
     * @param pool Pool to allocate from
     * @param count Number of elements
     * @param size Size of each element
     * @return Allocated zeroed memory or NULL
     */
    void *akira_pool_calloc(akira_pool_t *pool, size_t count, size_t size);

    /**
     * @brief Free memory back to pool
     * @param pool Pool to free to
     * @param ptr Memory to free
     */
    void akira_pool_free(akira_pool_t *pool, void *ptr);

    /**
     * @brief Get pool statistics
     * @param pool Pool to query
     * @param stats Output statistics
     * @return 0 on success
     */
    int akira_pool_stats(akira_pool_t *pool, akira_mem_stats_t *stats);

    /**
     * @brief Get pool free count (for fixed pools)
     * @param pool Pool to query
     * @return Number of free blocks
     */
    int akira_pool_free_count(akira_pool_t *pool);

    /*===========================================================================*/
    /* System Heap API                                                           */
    /*===========================================================================*/

    /**
     * @brief Allocate from system heap
     * @param size Size to allocate
     * @return Allocated memory or NULL
     */
    void *akira_malloc(size_t size);

    /**
     * @brief Allocate zeroed memory from system heap
     * @param count Number of elements
     * @param size Size of each element
     * @return Allocated zeroed memory or NULL
     */
    void *akira_calloc(size_t count, size_t size);

    /**
     * @brief Reallocate memory
     * @param ptr Original pointer
     * @param size New size
     * @return Reallocated memory or NULL
     */
    void *akira_realloc(void *ptr, size_t size);

    /**
     * @brief Free system heap memory
     * @param ptr Memory to free
     */
    void akira_free(void *ptr);

    /**
     * @brief Allocate aligned memory
     * @param alignment Alignment requirement
     * @param size Size to allocate
     * @return Aligned memory or NULL
     */
    void *akira_aligned_alloc(size_t alignment, size_t size);

    /**
     * @brief Free aligned memory
     * @param ptr Memory to free
     */
    void akira_aligned_free(void *ptr);

    /*===========================================================================*/
    /* Memory Region API                                                         */
    /*===========================================================================*/

    /**
     * @brief Create a memory region
     * @param size Region size
     * @param flags Protection flags
     * @return Region descriptor or NULL
     */
    akira_mem_region_t *akira_region_create(size_t size, uint32_t flags);

    /**
     * @brief Destroy a memory region
     * @param region Region to destroy
     */
    void akira_region_destroy(akira_mem_region_t *region);

    /**
     * @brief Change region protection
     * @param region Region to modify
     * @param flags New protection flags
     * @return 0 on success
     */
    int akira_region_protect(akira_mem_region_t *region, uint32_t flags);

    /**
     * @brief Map region into process address space
     * @param region Region to map
     * @param pid Target process
     * @return 0 on success
     */
    int akira_region_map(akira_mem_region_t *region, akira_pid_t pid);

    /**
     * @brief Unmap region from process
     * @param region Region to unmap
     * @param pid Target process
     * @return 0 on success
     */
    int akira_region_unmap(akira_mem_region_t *region, akira_pid_t pid);

    /*===========================================================================*/
    /* Statistics & Debugging                                                    */
    /*===========================================================================*/

    /**
     * @brief Get global memory statistics
     * @param stats Output statistics
     * @return 0 on success
     */
    int akira_memory_stats(akira_mem_stats_t *stats);

    /**
     * @brief Get process memory usage
     * @param pid Process ID
     * @return Memory usage in bytes
     */
    size_t akira_memory_usage(akira_pid_t pid);

    /**
     * @brief Check for memory leaks
     * @return Number of leaked allocations
     */
    int akira_memory_check_leaks(void);

    /**
     * @brief Dump memory state for debugging
     */
    void akira_memory_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_KERNEL_MEMORY_H */
