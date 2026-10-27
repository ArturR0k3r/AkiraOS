/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Registers every AKIRA_NATIVE_API_DEFINE() table with WAMR.
 *
 * Tables of the same import module are copied into one writable array and
 * registered as a single WAMR node: WAMR sorts each registered array in place
 * and keeps the pointer for the lifetime of the runtime, and one node per module
 * keeps import lookup to a single binary search. The copy lives in PSRAM when
 * the board has it, otherwise in a static pool sized by
 * CONFIG_AKIRA_NATIVE_REGISTRY_MAX_SYMBOLS.
 */

#include <akira_native_registry.h>

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lib/mem_helper.h>

LOG_MODULE_REGISTER(akira_native_registry, CONFIG_AKIRA_LOG_LEVEL);

STRUCT_SECTION_START_EXTERN(akira_native_api);
STRUCT_SECTION_END_EXTERN(akira_native_api);

#if !defined(CONFIG_AKIRA_PSRAM)
static NativeSymbol symbol_pool[CONFIG_AKIRA_NATIVE_REGISTRY_MAX_SYMBOLS];
static size_t symbol_pool_used;
#endif

static NativeSymbol *alloc_symbols(size_t count)
{
#if defined(CONFIG_AKIRA_PSRAM)
    return akira_malloc_buffer(count * sizeof(NativeSymbol));
#else
    if (count > ARRAY_SIZE(symbol_pool) - symbol_pool_used) {
        LOG_ERR("Native registry pool full: raise CONFIG_AKIRA_NATIVE_REGISTRY_MAX_SYMBOLS "
                "(%d) to at least %zu", CONFIG_AKIRA_NATIVE_REGISTRY_MAX_SYMBOLS,
                symbol_pool_used + count);
        return NULL;
    }
    NativeSymbol *symbols = &symbol_pool[symbol_pool_used];

    symbol_pool_used += count;
    return symbols;
#endif
}

/* Report every (module, name) pair defined more than once, within one table or
 * across tables. WAMR would silently let the newest definition win. */
static int count_duplicates(void)
{
    const struct akira_native_api *end = STRUCT_SECTION_END(akira_native_api);
    int duplicates = 0;

    STRUCT_SECTION_FOREACH(akira_native_api, api) {
        for (uint32_t i = 0; i < api->count; i++) {
            const char *name = api->symbols[i].symbol;

            for (uint32_t j = i + 1; j < api->count; j++) {
                if (strcmp(name, api->symbols[j].symbol) == 0) {
                    LOG_ERR("Native %s.%s is defined twice in %s", api->module, name,
                            api->name);
                    duplicates++;
                }
            }
            for (const struct akira_native_api *other = api + 1; other < end; other++) {
                if (strcmp(api->module, other->module) != 0) {
                    continue;
                }
                for (uint32_t k = 0; k < other->count; k++) {
                    if (strcmp(name, other->symbols[k].symbol) == 0) {
                        LOG_ERR("Native %s.%s is defined by both %s and %s", api->module,
                                name, api->name, other->name);
                        duplicates++;
                    }
                }
            }
        }
    }
    return duplicates;
}

static int register_module(const struct akira_native_api *first)
{
    const struct akira_native_api *end = STRUCT_SECTION_END(akira_native_api);
    size_t count = 0;
    bool optional = true;

    for (const struct akira_native_api *api = first; api < end; api++) {
        if (strcmp(api->module, first->module) == 0) {
            count += api->count;
            optional = optional && (api->flags & AKIRA_NATIVE_API_OPTIONAL);
        }
    }
    if (count == 0) {
        return 0;
    }

    NativeSymbol *symbols = alloc_symbols(count);

    if (!symbols) {
        LOG_ERR("No memory for %zu natives of module %s", count, first->module);
        return optional ? 0 : -ENOMEM;
    }

    size_t offset = 0;

    for (const struct akira_native_api *api = first; api < end; api++) {
        if (strcmp(api->module, first->module) == 0) {
            memcpy(&symbols[offset], api->symbols, api->count * sizeof(NativeSymbol));
            offset += api->count;
        }
    }

    if (!wasm_runtime_register_natives(first->module, symbols, (uint32_t)count)) {
        if (optional) {
            LOG_WRN("WAMR rejected optional module %s (%zu natives)", first->module, count);
            return 0;
        }
        LOG_ERR("WAMR rejected module %s (%zu natives)", first->module, count);
        return -EIO;
    }

    LOG_INF("Registered %zu natives for module %s", count, first->module);
    return 0;
}

int akira_native_registry_register_all(void)
{
    static K_MUTEX_DEFINE(lock);
    static bool done;
    static int result;

    k_mutex_lock(&lock, K_FOREVER);
    if (done) {
        k_mutex_unlock(&lock);
        return result;
    }

    if (count_duplicates() > 0) {
        result = -EEXIST;
        goto out;
    }

    result = 0;
    STRUCT_SECTION_FOREACH(akira_native_api, api) {
        bool registered = false;

        /* Each module is registered once, with its first table. */
        for (const struct akira_native_api *prev = STRUCT_SECTION_START(akira_native_api);
             prev < api; prev++) {
            if (strcmp(prev->module, api->module) == 0) {
                registered = true;
                break;
            }
        }
        if (registered) {
            continue;
        }

        result = register_module(api);
        if (result < 0) {
            break;
        }
    }

out:
    done = true;
    k_mutex_unlock(&lock);
    return result;
}
