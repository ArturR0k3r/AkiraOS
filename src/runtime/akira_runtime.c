
/*
 * src/services/akira_runtime.c
 * Unified Akira Runtime - direct-to-WAMR implementation for Minimalist Arch.
 *
 * Implements: init, load, start, stop, capability guard, and native bridge.
 */

#include "akira_runtime.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdint.h>
#include <zephyr/fs/fs.h>
#include <drivers/psram.h>
#include "platform_hal.h"
#include "storage/fs_manager.h"

#include <runtime/security.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#include <lib/simple_json.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(akira_runtime, CONFIG_AKIRA_LOG_LEVEL);

#define FILE_DIR_MAX_LEN 128

/* Internal managed app structure */
typedef struct {
    bool used;
    char name[32];
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t exec_env;
    bool running;
    uint32_t cap_mask; /* capability bitmask parsed from manifest */
} akira_managed_app_t;

static akira_managed_app_t g_apps[AKIRA_MAX_WASM_INSTANCES];

/* Capability bits */
#define CAP_DISPLAY_WRITE   (1U << 0)
#define CAP_INPUT_READ      (1U << 1)
#define CAP_SENSOR_READ     (1U << 2)
#define CAP_RF_TRANSCEIVE   (1U << 3)
static bool g_runtime_initialized = false;
static void *g_wamr_heap_buf = NULL;
static size_t g_wamr_heap_size = 0;

/* Forward native function prototypes (registered below) */
static int akira_native_display_clear(wasm_exec_env_t exec_env, uint32_t color);
static int akira_native_display_pixel(wasm_exec_env_t exec_env, int32_t x, int32_t y, uint32_t color);
static int akira_native_input_read_buttons(wasm_exec_env_t exec_env);
static int akira_native_rf_send(wasm_exec_env_t exec_env, uint32_t payload_ptr, uint32_t len);
static int akira_native_sensor_read(wasm_exec_env_t exec_env, int32_t type);
static int akira_native_log(wasm_exec_env_t exec_env, uint32_t level, char* message);

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

    if (akira_psram_available())
    {
        g_wamr_heap_buf = akira_psram_alloc(g_wamr_heap_size);
        if (g_wamr_heap_buf)
        {
            LOG_INF("Allocated WAMR heap in PSRAM: %zu bytes", g_wamr_heap_size);
        }
        if(g_wamr_heap_buf==NULL){
            LOG_WRN("PSRAM allocation for WAMR heap failed, falling back to internal RAM");
        }
        else{
            LOG_INF("Using PSRAM for WAMR heap");
        }
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

    RuntimeInitArgs init_args = {0};
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = g_wamr_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = g_wamr_heap_size;

    if (!wasm_runtime_full_init(&init_args))
    {
        LOG_ERR("WAMR runtime initialization failed");
        return -ENODEV;
    }

    /* Register native symbols (single registration point) */
    static NativeSymbol native_syms[] = {
        {"log", (void *)akira_native_log, "(i$)i", NULL},
        {"display_clear", (void *)akira_native_display_clear, "(i)i", NULL},
        {"display_pixel", (void *)akira_native_display_pixel, "(iii)i", NULL},
        {"input_read_buttons", (void *)akira_native_input_read_buttons, "()i", NULL},
        {"rf_send", (void *)akira_native_rf_send, "(*i)i", NULL},
        {"sensor_read", (void *)akira_native_sensor_read, "(i)i", NULL},
    };

    wasm_runtime_register_natives("akira", native_syms, sizeof(native_syms) / sizeof(NativeSymbol));

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

/* Load WASM binary into runtime (keeps loaded module in memory) */
int akira_runtime_load_wasm(const uint8_t *buffer, uint32_t size)
{
    if (!g_runtime_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (!buffer || size < 4 || memcmp(buffer, "\0asm", 4) != 0)
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
    wasm_module_t module = wasm_runtime_load((uint8_t *)buffer, size, NULL, 0);
    if (!module)
    {
        LOG_ERR("wasm_runtime_load failed: %s", wasm_runtime_get_exception(NULL));
        return -EIO;
    }

    g_apps[slot].used = true;
    g_apps[slot].module = module;
    g_apps[slot].running = false;
    g_apps[slot].cap_mask = 0; /* default no extra caps */
    snprintf(g_apps[slot].name, sizeof(g_apps[slot].name), "app%d", slot);

    LOG_INF("WASM module loaded into slot %d", slot);
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

/* Persistent install with manifest parsing */
static uint32_t parse_manifest_to_mask(const char *json, size_t json_len)
{
    /* Use our lightweight JSON parser (no external deps) that extracts
     * "capabilities": ["...", ...] and maps to capability mask bits.
     */
    return (uint32_t)parse_capabilities_mask(json, json_len);
}

int akira_runtime_install_with_manifest(const char *name, const void *binary, size_t size, const char *manifest_json, size_t manifest_size)
{
    if (!name || !binary || size == 0) return -EINVAL;

    /* Save manifest if provided */
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

    /* Load into runtime memory and set cap mask from manifest */
    int id = akira_runtime_load_wasm((const uint8_t *)binary, (uint32_t)size);
    if (id < 0) return id;

    if (manifest_json && manifest_size > 0) {
        uint32_t mask = parse_manifest_to_mask(manifest_json, manifest_size);
        g_apps[id].cap_mask = mask;
        LOG_INF("App %s installed with cap_mask=0x%08x", name, mask);
    } else {
        g_apps[id].cap_mask = 0; /* no special caps */
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

    /* Remove files */
    char path[FILE_DIR_MAX_LEN];
    snprintf(path, sizeof(path), "/lfs/apps/%s.wasm", name);
    int ret = fs_manager_delete_file(path);
    if(ret < 0){
        LOG_WRN("Failed to delete %s", path);
    }
    snprintf(path, sizeof(path), "/lfs/apps/%s.manifest.json", name);
    fs_manager_delete_file(path);
    if(ret < 0){
        LOG_WRN("Failed to delete %s", path);
    }

    return 0;
}

/* ===== Native functions (WASM-exposed) ===== */

//TODO: Change these into their respective api files

static int akira_native_log(wasm_exec_env_t exec_env, uint32_t level, char* message){
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) return -1;

    switch (level)
    {
    case LOG_LEVEL_ERR:
        LOG_ERR("Logged from wasm app %s",message);
        break;
    case LOG_LEVEL_WRN:
        LOG_WRN("Logged from wasm app %s",message);
        break;
    case LOG_LEVEL_INF:
        LOG_INF("Logged from wasm app %s",message);
        break;
    case LOG_LEVEL_DBG:
        LOG_DBG("Logged from wasm app %s",message);
        break;
    default:
        LOG_INF("UNKOWN TYPE pushed from wasm app (%d)", level);
        break;
    }

    return 0;
}

static int akira_native_display_clear(wasm_exec_env_t exec_env, uint32_t color)
{
    if (!akira_security_check_exec(exec_env, "display.write"))
        return -1;

#if AKIRA_PLATFORM_NATIVE_SIM
    /* On native_sim use sim helper */
    for (int y = 0; y < 320; y++)
        for (int x = 0; x < 240; x++)
            akira_sim_draw_pixel(x, y, (uint16_t)color);
    akira_sim_show_display();
    return 0;
#else
    /* On hardware platforms try framebuffer when available */
    uint16_t *fb = akira_framebuffer_get();
    if (fb)
    {
        for (int i = 0; i < 240 * 320; i++)
            fb[i] = (uint16_t)color;
        return 0;
    }
    return 0; /* No-op if display unavailable */
#endif
}

static int akira_native_display_pixel(wasm_exec_env_t exec_env, int32_t x, int32_t y, uint32_t color)
{
    if (!akira_security_check_exec(exec_env, "display.write"))
        return -1;

    if (x < 0 || x >= 240 || y < 0 || y >= 320)
        return -1;

#if AKIRA_PLATFORM_NATIVE_SIM
    akira_sim_draw_pixel(x, y, (uint16_t)color);
    return 0;
#else
    uint16_t *fb = akira_framebuffer_get();
    if (fb)
    {
        fb[y * 240 + x] = (uint16_t)color;
        return 0;
    }
    return -1;
#endif
}

static int akira_native_input_read_buttons(wasm_exec_env_t exec_env)
{
    if (!akira_security_check_exec(exec_env, "input.read"))
        return 0;

#if AKIRA_PLATFORM_NATIVE_SIM
    return (int)akira_sim_read_buttons();
#else
    /* No generic platform input API - return 0 by default */
    return 0;
#endif
}

static int akira_native_rf_send(wasm_exec_env_t exec_env, uint32_t payload_ptr, uint32_t len)
{
#ifdef CONFIG_AKIRA_WASM_RUNTIME
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;

    if (!akira_security_check_exec(exec_env, "rf.transceive"))
        return -1;

    if (len == 0)
        return -1;

    uint8_t *ptr = (uint8_t *)wasm_runtime_addr_app_to_native(module_inst, payload_ptr);
    if (!ptr)
        return -1;

#ifdef CONFIG_AKIRA_RF_FRAMEWORK
    /* The API layer will dispatch to the proper radio driver */
    return akira_rf_send(ptr, len);
#else
    (void)ptr; (void)len;
    return -ENOSYS;
#endif
#else
    (void)exec_env; (void)payload_ptr; (void)len;
    /* WAMR disabled, cannot map pointers nor enforce per-exec caps */
    if (!akira_security_check_native("rf.transceive"))
        return -1;
    return -ENOSYS;
#endif
}

static int akira_native_sensor_read(wasm_exec_env_t exec_env, int32_t type)
{
#ifdef CONFIG_AKIRA_WASM_RUNTIME
    if (!akira_security_check_exec(exec_env, "sensor.read"))
        return -1;
#else
    if (!akira_security_check_native("sensor.read"))
        return -1;
    (void)exec_env;
#endif

#ifdef CONFIG_AKIRA_BME280
    {
        float v = 0.0f;
        if (akira_sensor_read((akira_sensor_type_t)type, &v) == 0)
        {
            /* Convert float to int32 with simple scaling (milli-units) */
            return (int)(v * 1000.0f);
        }
    }
#endif
    (void)type;
    return -ENOSYS;
}

/* End of file */
