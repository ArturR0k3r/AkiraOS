#include "psram.h"
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_AKIRA_PSRAM
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <soc/soc_memory_layout.h>
#endif


static bool psram_initialized = false;

#define PSRAM_TEST_SIZE 1024

LOG_MODULE_REGISTER(akira_psram, CONFIG_AKIRA_LOG_LEVEL);

int akira_init_psram_heap(void) {
#ifdef CONFIG_AKIRA_PSRAM
    // Test PSRAM allocation to verify it's available
    void *test = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, PSRAM_TEST_SIZE);
    
    if (test == NULL) {
        return -1;
    }
    
    // Verify it's actually in external RAM
    if (!esp_ptr_external_ram(test)) {
        LOG_ERR("ERROR: Allocated memory is not in PSRAM! (%p)\n", test);
        shared_multi_heap_free(test);
        return -1;
    }
    
    LOG_INF("PSRAM initialized and available at %p\n", test);
    shared_multi_heap_free(test);
    psram_initialized = true;
    
    return 0;
#else
    //TODO: Implement PSRAM support for other platforms 
    /* PSRAM not supported on non-ESP32 platforms yet */
    return -ENOTSUP;
#endif
}

bool akira_psram_available(void)
{
#ifdef CONFIG_AKIRA_PSRAM
    return psram_initialized;
#else
    return false;
#endif
}

void *akira_psram_alloc(size_t size)
{
#if defined(CONFIG_AKIRA_PSRAM) 
    if (!psram_initialized) {
        LOG_ERR("ERROR: PSRAM not initialized!\n");
        return NULL;
    }
    
    // Allocate directly from PSRAM
    void *ptr = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, size);
    
    if (ptr == NULL) {
        LOG_ERR("ERROR: PSRAM allocation failed for size %zu\n", size);
    } else {
        LOG_INF("Allocated %zu bytes from PSRAM at %p\n", size, ptr);
    }
    
    return ptr;
#else
    (void)size;
    return NULL;
#endif
}

void akira_psram_free(void *ptr)
{
#if defined(CONFIG_AKIRA_PSRAM)
    if (ptr != NULL) {
        shared_multi_heap_free(ptr);
        LOG_INF("Freed PSRAM pointer %p\n", ptr);
    }
#else
    (void)ptr;
#endif
}