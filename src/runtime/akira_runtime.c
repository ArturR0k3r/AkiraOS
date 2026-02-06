
/*
 * src/services/akira_runtime.c
 * Unified Akira Runtime - direct-to-WAMR implementation for Minimalist Arch.
 *
 * Implements: init, load, start, stop, capability guard, and native bridge.
 * Optimized for low SRAM usage with chunked WASM loading and PSRAM fallback.
 */

#include "akira_runtime.h"
#include "manifest_parser.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdint.h>
#include <zephyr/fs/fs.h>
#include <lib/mem_helper.h>
#include "platform_hal.h"
#include "storage/fs_manager.h"
#include <lib/mem_helper.h>
#include <runtime/security.h>

#include "akira_api.h"

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#include <lib/simple_json.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(akira_runtime, CONFIG_AKIRA_LOG_LEVEL);

#define FILE_DIR_MAX_LEN 128

/* Chunked loading configuration */
#define CHUNK_BUFFER_SIZE   (16 * 1024)  /* 16KB chunks for WASM loading */

akira_managed_app_t g_apps[AKIRA_MAX_WASM_INSTANCES];

/* Use capability bits from security.h (AKIRA_CAP_*) */
static bool g_runtime_initialized = false;
static void *g_wamr_heap_buf = NULL;
static size_t g_wamr_heap_size = 0;

/* Capability checking is implemented in src/runtime/security.c */

/* Helper: find free slot */
static int find_free_slot(void)
{
    for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++)
    {
        if (!g_apps[i].used)
            return i;
    }
    return -ENOMEM;
}

/* Helper: find by id validity */
static bool slot_valid(int id)
{
    return (id >= 0 && id < AKIRA_MAX_WASM_INSTANCES && g_apps[id].used);
}

/* Runtime helpers used by security.c */
uint32_t akira_runtime_get_cap_mask_for_module_inst(wasm_module_inst_t inst)
{
    if (!inst) return 0;
    for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++) {
        if (g_apps[i].used && g_apps[i].instance == inst) return g_apps[i].cap_mask;
    }
    return 0;
}

int akira_runtime_get_name_for_module_inst(wasm_module_inst_t inst, char *buf, size_t buflen)
{
    if (!inst || !buf || buflen == 0) return -EINVAL;
    for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++) {
        if (g_apps[i].used && g_apps[i].instance == inst) {
            strncpy(buf, g_apps[i].name, buflen-1);
            buf[buflen-1] = '\0';
            return 0;
        }
    }
    return -ENOENT;
}

/* Get app slot from module instance - used by quota enforcement */
int get_slot_for_module_inst(wasm_module_inst_t inst)
{
    if (!inst) return -1;
    for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++) {
        if (g_apps[i].used && g_apps[i].instance == inst) {
            return i;
        }
    }
    return -1;
}

/*
 * WASM Memory Allocation with Per-App Quota Enforcement
 *
 * These functions provide quota-aware memory allocation for WASM apps.
 * They use PSRAM-preferred allocation and enforce per-app memory limits.
 *
 * Design:
 * - Atomic quota tracking for thread safety
 * - Graceful NULL return on quota violation (no crash)
 * - PSRAM-first allocation reduces SRAM pressure
 * - Header stores size for deallocation tracking
 */



/**
 * @brief Get current memory usage for an app
 *
 * @param instance_id  App instance ID
 * @return Current memory usage in bytes, or 0 if invalid
 */
uint32_t akira_runtime_get_memory_used(int instance_id)
{
    if (!slot_valid(instance_id)) {
        return 0;
    }
    return g_apps[instance_id].memory_used;
}

/**
 * @brief Get memory quota for an app
 *
 * @param instance_id  App instance ID
 * @return Memory quota in bytes (0 = unlimited), or 0 if invalid
 */
uint32_t akira_runtime_get_memory_quota(int instance_id)
{
    if (!slot_valid(instance_id)) {
        return 0;
    }
    return g_apps[instance_id].memory_quota;
}

/* Initialize WAMR runtime with PSRAM-backed heap when available */
int akira_runtime_init(void)
{
    if (g_runtime_initialized)
        return 0;

    LOG_INF("Initializing Akira unified runtime...");

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    /* Prefer PSRAM for WAMR heap if available */
#ifdef CONFIG_WAMR_HEAP_SIZE
    g_wamr_heap_size = (size_t)CONFIG_WAMR_HEAP_SIZE;
#else
    g_wamr_heap_size = (256 * 1024); /* reasonable default */
#endif

    g_wamr_heap_buf = akira_malloc_buffer(g_wamr_heap_size);
    if (g_wamr_heap_buf)
    {
        LOG_INF("Allocated WAMR heap in PSRAM: %zu bytes", g_wamr_heap_size);
    }
    else{
        LOG_WRN("PSRAM allocation for WAMR heap failed, falling back to internal RAM");
    }

    if (!g_wamr_heap_buf)
    {
#ifndef CONFIG_AKIRA_PSRAM
#ifndef CONFIG_WAMR_HEAP_SIZE
#define AKIRA_WAMR_HEAP_DEFAULT (262144)
#else
#define AKIRA_WAMR_HEAP_DEFAULT (CONFIG_WAMR_HEAP_SIZE)
#endif
        static uint8_t internal_wamr_heap[AKIRA_WAMR_HEAP_DEFAULT];
        g_wamr_heap_buf = internal_wamr_heap;
        g_wamr_heap_size = sizeof(internal_wamr_heap);
        LOG_INF("Using internal WAMR heap: %zu bytes", g_wamr_heap_size);
#else
        /* If AKIRA_PSRAM is enabled but allocation failed at runtime, try to allocate
         * dynamically to avoid reserving large static buffers in DRAM. This keeps
         * the binary small while still allowing large heaps when PSRAM is present.
         */
        g_wamr_heap_size = (32 * 1024); /* fallback smaller size */
        g_wamr_heap_buf = k_malloc(g_wamr_heap_size);
        if (g_wamr_heap_buf) {
            LOG_INF("Allocated WAMR heap dynamically: %zu bytes", g_wamr_heap_size);
        } else {
            LOG_ERR("Failed to allocate WAMR heap dynamically");
            return -ENOMEM;
        }
#endif
    }

    /*
     * WAMR runtime initialization with custom allocator hooks
     *
     * We use Alloc_With_Pool for the global WAMR heap (efficient for
     * module loading), but provide native malloc/free APIs for WASM apps
     * that need quota-enforced dynamic allocation.
     *
     */
    RuntimeInitArgs init_args = {0};
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = g_wamr_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = g_wamr_heap_size;

    if (!wasm_runtime_full_init(&init_args))
    {
        LOG_ERR("WAMR runtime initialization failed");
        return -ENODEV;
    }

    #ifdef CONFIG_AKIRA_WASM_API
    if(!akira_register_native_apis()){
        LOG_ERR("Failed to register native APIs");
        return -EIO;
    }
    #else
    LOG_WRN("Native API registration not included - no APIs enabled (CONFIG_AKIRA_WASM_API not set)");
    #endif /* CONFIG_AKIRA_WASM_API */

    /* Ensure WASM apps dir exists */
    if(fs_manager_exists("/lfs/apps") != 1){ // Not found
        fs_manager_mkdir("/lfs/apps");
    }

    g_runtime_initialized = true;
    LOG_INF("Akira runtime initialized (WAMR + native bridge)");
    return 0;
#else
    /* No runtime available when WAMR is disabled. To enable, set CONFIG_AKIRA_WASM_RUNTIME
     * and ensure WAMR integration is available for the target.
     */
    LOG_ERR("WASM support disabled - runtime not enabled (CONFIG_AKIRA_WASM_RUNTIME)");
    return -ENOTSUP;
#endif
}

/* Load WASM binary into runtime using chunked loading to reduce peak SRAM
 * The WASM binary is processed in 16KB chunks, with the chunk buffer
 * allocated from PSRAM when available to minimize SRAM pressure.
 */
int akira_runtime_load_wasm(const uint8_t *buffer, uint32_t size)
{
    if (!g_runtime_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (!buffer || size < 8 || memcmp(buffer, "\0asm", 4) != 0)
    {
        LOG_ERR("Invalid WASM binary");
        return -EINVAL;
    }

    int slot = find_free_slot();
    if (slot < 0)
    {
        LOG_ERR("No free slots for WASM modules");
        return slot;
    }

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    /* Parse manifest from WASM custom section before loading */
    akira_manifest_t manifest;
    manifest_init_defaults(&manifest);
    int manifest_ret = manifest_parse_wasm_section(buffer, size, &manifest);
    if (manifest_ret == 0) {
        LOG_INF("Found embedded manifest: cap_mask=0x%08x, memory_quota=%u",
                manifest.cap_mask, manifest.memory_quota);
    }

    /* Allocate chunk buffer from PSRAM if available, else SRAM */
    mem_source_t chunk_src;
    uint8_t *chunk_buffer = akira_malloc_buffer_ex(CHUNK_BUFFER_SIZE, &chunk_src);
    if (!chunk_buffer)
    {
        LOG_ERR("Failed to allocate chunk buffer (%d bytes)", CHUNK_BUFFER_SIZE);
        return -ENOMEM;
    }
    LOG_INF("Chunk buffer allocated from %s (%d bytes)",
            chunk_src == MEM_SOURCE_PSRAM ? "PSRAM" : "SRAM", CHUNK_BUFFER_SIZE);

    /* Use WAMR's streaming loader for chunked loading when available,
     * otherwise fall back to direct load with our buffer management.
     *
     * For WAMR interpreter mode, wasm_runtime_load() requires the full
     * binary, but we use our PSRAM-backed buffer to stage it.
     */
    wasm_module_t module = NULL;
    char error_buf[128] = {0};

    /* For large WASM files, copy to PSRAM-backed buffer if the source
     * is in constrained memory. This trades one-time copy for reduced
     * peak SRAM during the WAMR load/parse phase.
     */
    const uint8_t *load_buffer = buffer;
    uint8_t *staged_buffer = NULL;

    if (size > CHUNK_BUFFER_SIZE && chunk_src == MEM_SOURCE_PSRAM)
    {
        /* Stage the entire WASM to PSRAM in chunks to reduce SRAM pressure */
        staged_buffer = akira_malloc_buffer(size);
        if (staged_buffer)
        {
            LOG_INF("Staging %u bytes WASM to external memory", size);

            /* Copy in chunks to avoid large stack/heap spikes */
            uint32_t offset = 0;
            while (offset < size)
            {
                uint32_t chunk_len = MIN(CHUNK_BUFFER_SIZE, size - offset);
                memcpy(chunk_buffer, buffer + offset, chunk_len);
                memcpy(staged_buffer + offset, chunk_buffer, chunk_len);
                offset += chunk_len;
            }
            load_buffer = staged_buffer;
            LOG_INF("WASM staged to PSRAM successfully");
        }
        else
        {
            LOG_WRN("Could not stage to PSRAM, loading from original buffer");
        }
    }

    /* Load the WASM module */
    module = wasm_runtime_load((uint8_t *)load_buffer, size, error_buf, sizeof(error_buf));

    /* Free chunk buffer immediately after load */
    akira_free_buffer(chunk_buffer);
    chunk_buffer = NULL;

    /* Free staged buffer if we used one */
    if (staged_buffer)
    {
        akira_free_buffer(staged_buffer);
        staged_buffer = NULL;
    }

    if (!module)
    {
        LOG_ERR("wasm_runtime_load failed: %s", error_buf);
        return -EIO;
    }

    g_apps[slot].used = true;
    g_apps[slot].module = module;
    g_apps[slot].running = false;
    g_apps[slot].cap_mask = manifest.valid ? manifest.cap_mask : 0;
    g_apps[slot].memory_quota = manifest.valid ? manifest.memory_quota : 0;
    g_apps[slot].memory_used = 0;  /* Reset memory usage for new app */
    snprintf(g_apps[slot].name, sizeof(g_apps[slot].name),
             manifest.valid && manifest.name[0] ? manifest.name : "app%d", slot);

    LOG_INF("WASM module loaded into slot %d (cap_mask=0x%08x, quota=%u)",
            slot, g_apps[slot].cap_mask, g_apps[slot].memory_quota);
    return slot;
#else
    (void)buffer; (void)size;
    return -ENOTSUP; /* WASM runtime not available */
#endif
}

/* Instantiate and run a loaded WASM module */
int akira_runtime_start(int instance_id)
{
    if (!slot_valid(instance_id))
        return -EINVAL;

    akira_managed_app_t *app = &g_apps[instance_id];
    if (app->running)
        return 0;

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#ifndef CONFIG_WAMR_INSTANCE_HEAP
#define CONFIG_WAMR_INSTANCE_HEAP 65536
#endif
#ifndef CONFIG_WAMR_STACK_SIZE
#define CONFIG_WAMR_STACK_SIZE 8192
#endif
    wasm_module_inst_t inst = wasm_runtime_instantiate(app->module, CONFIG_WAMR_INSTANCE_HEAP, CONFIG_WAMR_STACK_SIZE, NULL, 0);
    if (!inst)
    {
        LOG_ERR("wasm_runtime_instantiate failed: %s", wasm_runtime_get_exception(NULL));
        return -EIO;
    }

    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst, CONFIG_WAMR_STACK_SIZE);
    if (!exec_env)
    {
        LOG_ERR("Failed to create exec env");
        wasm_runtime_deinstantiate(inst);
        return -ENOMEM;
    }

    app->instance = inst;
    app->exec_env = exec_env;

    /* Call _start or main if present */
    wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, "_start");
    if (!fn)
        fn = wasm_runtime_lookup_function(inst, "main");
    if (fn)
    {
        uint32_t argv[2]; // empty 
        if (!wasm_runtime_call_wasm(exec_env, fn, 2, argv))
        {
            LOG_ERR("WASM start exception: %s", wasm_runtime_get_exception(inst));
        }
    }
    else if(!fn){
        LOG_WRN("No _start or main function found in WASM module");
        wasm_runtime_deinstantiate(inst);
        wasm_runtime_destroy_exec_env(exec_env);
        return -ENOENT;
    }

    app->running = true;
    LOG_INF("Started instance %d", instance_id);
    return 0;
#else
    (void)instance_id;
    return -ENOTSUP; /* WASM runtime not available */
#endif
}

/* Stop and deinstantiate an instance */
int akira_runtime_stop(int instance_id)
{
    if (!slot_valid(instance_id))
        return -EINVAL;

    akira_managed_app_t *app = &g_apps[instance_id];
    if (!app->running && !app->instance)
        return 0;

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    if (app->instance)
    {
        wasm_runtime_deinstantiate(app->instance);
        app->instance = NULL;
    }

    if (app->exec_env)
    {
        wasm_runtime_destroy_exec_env(app->exec_env);
        app->exec_env = NULL;
    }

    app->running = false;
    LOG_INF("Stopped instance %d", instance_id);
    return 0;
#else
    (void)instance_id;
    return -ENOTSUP; /* WASM runtime not available */
#endif
}

int akira_runtime_install_with_manifest(const char *name, const void *binary, size_t size, const char *manifest_json, size_t manifest_size)
{
    if (!name || !binary || size == 0) return -EINVAL;

    /* Parse manifest with fallback: WASM section first, then JSON */
    akira_manifest_t manifest;
    manifest_parse_with_fallback((const uint8_t *)binary, size,
                                 manifest_json, manifest_size, &manifest);

    /* Save manifest if provided (for external tools/debugging) */
    if (manifest_json && manifest_size > 0) {
        char mpath[FILE_DIR_MAX_LEN];
        snprintf(mpath, sizeof(mpath), "/lfs/apps/%s.manifest.json", name);
        if (fs_manager_exists(mpath) ) {
            ssize_t mv = fs_manager_write_file(mpath, manifest_json, manifest_size);
            if (mv != (ssize_t)manifest_size) {
                LOG_WRN("Failed to write manifest fully for %s", name);
            } else {
                LOG_INF("Saved manifest to %s", mpath);
            }
        } else {
            LOG_WRN("Filesystem not available for manifest save");
        }
    }

    /* Load into runtime memory - this will also parse embedded manifest */
    int id = akira_runtime_load_wasm((const uint8_t *)binary, (uint32_t)size);
    if (id < 0) return id;

    /* Override with external manifest if it has more capabilities */
    if (manifest.valid && manifest.cap_mask != 0) {
        g_apps[id].cap_mask |= manifest.cap_mask;
        if (manifest.memory_quota > 0) {
            g_apps[id].memory_quota = manifest.memory_quota;
        }
        LOG_INF("App %s: merged manifest cap_mask=0x%08x, memory_quota=%u",
                name, g_apps[id].cap_mask, g_apps[id].memory_quota);
    }

    /* Store friendly name */
    strncpy(g_apps[id].name, name, sizeof(g_apps[id].name)-1);
    g_apps[id].name[sizeof(g_apps[id].name)-1] = '\0';

    return id;
}

int akira_runtime_install(const char *name, const void *binary, size_t size)
{
    return akira_runtime_install_with_manifest(name, binary, size, NULL, 0);
}

/* Destroy: fully deinstantiate module and free its slot */
int akira_runtime_destroy(int instance_id)
{
    if (!slot_valid(instance_id)) return -EINVAL;

    akira_managed_app_t *app = &g_apps[instance_id];

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    if (app->instance)
    {
        wasm_runtime_deinstantiate(app->instance);
        app->instance = NULL;
    }

    if (app->exec_env)
    {
        wasm_runtime_destroy_exec_env(app->exec_env);
        app->exec_env = NULL;
    }

    if (app->module)
    {
        wasm_runtime_unload(app->module);
        app->module = NULL;
    }
#endif

    app->used = false;
    app->running = false;
    app->cap_mask = 0;
    app->name[0] = '\0';
    return 0;
}

/* Uninstall: remove persistent files and destroy runtime slot */
int akira_runtime_uninstall(const char *name, int instance_id)
{
    if (!name) return -EINVAL;

    /* Stop/destroy if running */
    if (instance_id >= 0)
    {
        akira_runtime_stop(instance_id);
        akira_runtime_destroy(instance_id);
    }

    return 0;
}

/* End of file */
