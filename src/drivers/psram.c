#include "psram.h"
#include <stdlib.h>

bool akira_psram_available(void)
{
#ifdef CONFIG_AKIRA_PSRAM
    return true;
#else
    return false;
#endif
}

void *akira_psram_alloc(size_t size)
{
#ifdef CONFIG_AKIRA_PSRAM
    /* Placeholder: in real hardware integrate PSRAM allocator/heap here */
    return malloc(size);
#else
    (void)size;
    return NULL;
#endif
}

void akira_psram_free(void *ptr)
{
#ifdef CONFIG_AKIRA_PSRAM
    free(ptr);
#else
    (void)ptr;
#endif
}
