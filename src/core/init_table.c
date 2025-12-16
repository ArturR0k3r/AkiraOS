/**
 * @file init_table.c
 * @brief Subsystem Initialization Table Implementation
 */

#include "init_table.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(init_table, CONFIG_AKIRA_LOG_LEVEL);

#define MAX_SUBSYSTEMS 64

static struct {
    subsystem_entry_t subsystems[MAX_SUBSYSTEMS];
    int count;
    int success_count;
    int failed_count;
    bool initialized;
} init_state = {0};

int init_table_register(const char *name,
                       init_priority_t priority,
                       subsystem_init_fn init_fn,
                       bool required,
                       bool enabled)
{
    if (!name || !init_fn) {
        return -EINVAL;
    }
    
    if (init_state.count >= MAX_SUBSYSTEMS) {
        LOG_ERR("Init table full, cannot register: %s", name);
        return -ENOMEM;
    }
    
    subsystem_entry_t *entry = &init_state.subsystems[init_state.count];
    entry->name = name;
    entry->priority = priority;
    entry->init_fn = init_fn;
    entry->required = required;
    entry->enabled = enabled;
    
    init_state.count++;
    
    LOG_DBG("Registered subsystem: %s (priority=%d, enabled=%d)", 
            name, priority, enabled);
    
    return 0;
}

static int compare_priority(const void *a, const void *b)
{
    const subsystem_entry_t *entry_a = (const subsystem_entry_t *)a;
    const subsystem_entry_t *entry_b = (const subsystem_entry_t *)b;
    
    /* Sort by priority (lower first) */
    return entry_a->priority - entry_b->priority;
}

int init_table_run(void)
{
    if (init_state.initialized) {
        LOG_WRN("Init table already run");
        return 0;
    }
    
    LOG_INF("════════════════════════════════════════");
    LOG_INF("  AkiraOS Subsystem Initialization");
    LOG_INF("════════════════════════════════════════");
    LOG_INF("Registered subsystems: %d", init_state.count);
    
    /* Sort by priority */
    qsort(init_state.subsystems, init_state.count, 
          sizeof(subsystem_entry_t), compare_priority);
    
    init_state.success_count = 0;
    init_state.failed_count = 0;
    
    /* Initialize each subsystem */
    for (int i = 0; i < init_state.count; i++) {
        subsystem_entry_t *entry = &init_state.subsystems[i];
        
        if (!entry->enabled) {
            LOG_DBG("[SKIP] %s (disabled)", entry->name);
            continue;
        }
        
        LOG_INF("Initializing: %s (priority=%d)%s", 
                entry->name, 
                entry->priority,
                entry->required ? " [REQUIRED]" : "");
        
        int ret = entry->init_fn();
        
        if (ret < 0) {
            LOG_ERR("❌ %s failed: %d", entry->name, ret);
            init_state.failed_count++;
            
            if (entry->required) {
                LOG_ERR("Required subsystem failed, aborting initialization");
                return ret;
            }
        } else {
            LOG_INF("✅ %s initialized", entry->name);
            init_state.success_count++;
        }
    }
    
    init_state.initialized = true;
    
    LOG_INF("════════════════════════════════════════");
    LOG_INF("Initialization complete:");
    LOG_INF("  Success: %d", init_state.success_count);
    LOG_INF("  Failed:  %d", init_state.failed_count);
    LOG_INF("════════════════════════════════════════");
    
    return 0;
}

void init_table_get_stats(int *total_count, int *success_count, int *failed_count)
{
    if (total_count) {
        *total_count = init_state.count;
    }
    if (success_count) {
        *success_count = init_state.success_count;
    }
    if (failed_count) {
        *failed_count = init_state.failed_count;
    }
}
