#include "psram.h"
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <soc/soc_memory_layout.h> 

static bool psram_initialized = false;

#define PSRAM_TEST_SIZE 1024

int akira_init_psram_heap(void) {
    // Test PSRAM allocation to verify it's available
    void *test = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, PSRAM_TEST_SIZE);
    
    if (test == NULL) {
        return -1;
    }
    
    // Verify it's actually in external RAM
    if (!esp_ptr_external_ram(test)) {
        printk("ERROR: Allocated memory is not in PSRAM! (%p)\n", test);
        shared_multi_heap_free(test);
        return -1;
    }
    
    printk("PSRAM initialized and available at %p\n", test);
    shared_multi_heap_free(test);
    psram_initialized = true;
    
    return 0;
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
#ifdef CONFIG_AKIRA_PSRAM
    if (!psram_initialized) {
        printk("ERROR: PSRAM not initialized!\n");
        return NULL;
    }
    
    // Allocate directly from PSRAM
    void *ptr = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, size);
    
    if (ptr == NULL) {
        printk("ERROR: PSRAM allocation failed for size %zu\n", size);
    } else {
        printk("Allocated %zu bytes from PSRAM at %p\n", size, ptr);
    }
    
    return ptr;
#else
    (void)size;
    return NULL;
#endif
}

void akira_psram_free(void *ptr)
{
#ifdef CONFIG_AKIRA_PSRAM
    if (ptr != NULL) {
        shared_multi_heap_free(ptr);
        printk("Freed PSRAM pointer %p\n", ptr);
    }
#else
    (void)ptr;
#endif
}