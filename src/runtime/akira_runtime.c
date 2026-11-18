
/*
 * src/services/akira_runtime.c
 * Unified Akira Runtime - direct-to-WAMR implementation for Minimalist Arch.
 *
 * Implements: init, load, start, stop, capability guard, and native bridge.
 * Optimized for low SRAM usage with chunked WASM loading and PSRAM fallback.
 */

#include <akira_admission.h>
#include "akira_runtime.h"
#include "manifest_parser.h"
#include "akira_abi_check.h"
#include "runtime_cache.h"
#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdint.h>
#include <zephyr/fs/fs.h>
#include <lib/mem_helper.h>
#include "platform_hal.h"
#include "storage/fs_manager.h"
#include <runtime/security.h>
#include <runtime/security/sandbox.h>
#include <runtime/security/app_signing.h>

#include "akira_api.h"

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <akira_native_registry.h>
#endif

#ifdef CONFIG_AKIRA_WASM_IPC
#include <runtime/akira_ipc.h>
#endif

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#include <lib/simple_json.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(akira_runtime, CONFIG_AKIRA_LOG_LEVEL);

/* An app that drew to the screen owns it until teardown, so releasing on the
 * way out is part of instance cleanup.  akira_api.h only pulls in
 * akira_display_api.h when CONFIG_DISPLAY=y (and CMakeLists only compiles the
 * display API then), so headless boards need this to compile away entirely. */
#ifdef CONFIG_DISPLAY
#define AKIRA_DISPLAY_RELEASE_OWNER(inst) akira_display_release_owner(inst)
#else
#define AKIRA_DISPLAY_RELEASE_OWNER(inst) ((void)0)
#endif

#define FILE_DIR_MAX_LEN 128

/* Chunked loading configuration */
#define CHUNK_BUFFER_SIZE (16 * 1024) /* 16KB chunks for WASM loading */

akira_managed_app_t g_apps[AKIRA_MAX_WASM_INSTANCES];

/* Zephyr thread pool — one stack + control block per slot.
 *
 * IMPORTANT: stacks MUST live in internal SRAM, never PSRAM.
 *
 * On ESP32-S3 (and any board with external PSRAM on the SPI0 bus), a direct
 * flash write/erase issued by the LittleFS garbage collector (e.g. triggered
 * inside lfs_file_open) temporarily disables SPI0 cache access so the bus
 * can be used exclusively for the flash transaction.  If the running thread's
 * stack is in PSRAM (also on SPI0), the CPU cannot read its own stack frames
 * during that window, causing a hard fault / complete system freeze.
 *
 * Keeping stacks in SRAM avoids this entirely.  Only the WASM heap,
 * WASM binary, and other runtime data need to live in PSRAM.
 *
 * We allocate via k_thread_stack_alloc() in a SYS_INIT hook at APPLICATION
 * level 1, before BT/WiFi/USB consume and fragment the SRAM heap. Stacks are
 * held permanently and reused across app launches without re-allocation.
 * Size is tuned per-board via CONFIG_AKIRA_WASM_APP_STACK_SIZE.
 */
static k_thread_stack_t *g_app_stacks[AKIRA_MAX_WASM_INSTANCES];

/* Early SYS_INIT hook: allocates WASM thread stacks before BT/WiFi/USB init
 * fragment the heap. APPLICATION level 1 runs before all driver-level init
 * that would otherwise consume contiguous SRAM blocks. */
static int wasm_stacks_early_alloc(void)
{
    for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++)
    {
        g_app_stacks[i] = k_thread_stack_alloc(CONFIG_AKIRA_WASM_APP_STACK_SIZE, 0);
        /* Failure logged in akira_runtime_init() when the slot is first used. */
    }
    return 0;
}
SYS_INIT(wasm_stacks_early_alloc, APPLICATION, 1);
static struct k_thread g_app_threads[AKIRA_MAX_WASM_INSTANCES];
/* A thread was ever created on this slot's k_thread/stack. */
static bool g_app_thread_created[AKIRA_MAX_WASM_INSTANCES];

/* Per-slot deferred cleanup context.
 * When a WASM thread exits naturally (main() returns), we cannot free the
 * SRAM stack from within the exiting thread itself (use-after-free).  We
 * also cannot call k_thread_join from the dying thread (deadlock).  Instead,
 * we schedule a k_work item that runs in the system work queue — a separate
 * thread — which safely joins the dead thread and frees its 8 KB SRAM stack.
 * Without this, stacks accumulate in the SRAM kernel heap and exhaust it,
 * causing k_malloc failures inside the Zephyr SPI DMA driver when the next
 * display_write() is attempted. */
typedef struct
{
    struct k_work work;
    int slot;
} slot_cleanup_ctx_t;

static slot_cleanup_ctx_t g_slot_cleanup[AKIRA_MAX_WASM_INSTANCES];

static void slot_cleanup_work_fn(struct k_work *work)
{
    slot_cleanup_ctx_t *ctx = CONTAINER_OF(work, slot_cleanup_ctx_t, work);
    int slot = ctx->slot;

    /* Wait for the thread to fully terminate.  It should already be dead
     * (work is submitted right before wasm_app_thread_fn returns), but
     * a short timeout makes this robust against scheduling races. */
    k_thread_join(&g_app_threads[slot], K_MSEC(500));

    /* Static stack — pre-allocated at init and held permanently; no heap to
     * free. Slot is ready for reuse by the next app launch. */
    LOG_DBG("SRAM stack for slot %d ready for reuse", slot);
}

/* Protects g_apps[] slot reservation in load_wasm() so concurrent calls
 * cannot grab the same free slot simultaneously. Held very briefly
 * (find + used=true), never held during blocking operations.
 * Also protects g_exit_cb registration and g_runtime_initialized. */
static K_MUTEX_DEFINE(g_runtime_mutex);

/* Optional exit callback registered by app_manager.
 * Write protected by g_runtime_mutex; reads on the exit path hold the
 * per-slot app->exit_mutex so a concurrent set_exit_callback() cannot
 * produce a torn read on 64-bit platforms (native_sim). */
static akira_runtime_exit_cb_t g_exit_cb = NULL;
static akira_runtime_exit_cb_t get_exit_cb(void);

/* Atomic init flag: prevents double-init if two threads race at boot. */
static atomic_t g_runtime_initialized = ATOMIC_INIT(0);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/*
 * WAMR allocator trampolines — Alloc_With_Allocator mode.
 *
 * Each allocation carries a uint64_t header (8 bytes) encoding:
 *   low  32 bits = requested size
 *   high 32 bits = bits 0-7:  byte offset from the raw allocation to the header
 *                  bits 8-15: slot + 1 the allocation is charged to (0 = none)
 * The raw allocation is rounded up to the next 8-byte boundary so that
 * hdr+1 (the returned user pointer) is always 8-byte aligned regardless
 * of whether the underlying allocator returns 4- or 8-byte-aligned blocks.
 * WAMR asserts 8-byte alignment on the instance heap buffer — misalignment
 * produces the "heap init struct buf not 8-byte aligned" error.
 *
 * What WAMR allocates on an app's own thread (its instance, linear memory,
 * every memory.grow) is charged to that app and capped at
 * CONFIG_AKIRA_WASM_APP_MEM_LIMIT_KB, and comes from PSRAM only on boards
 * that have it: an app growing its memory fails its own allocation instead of
 * draining the internal SRAM that drivers and radios need. Module loading runs
 * on the installing thread and is not charged.
 */
#define WAMR_HDR_SLOT_SHIFT 8
#define WAMR_APP_MEM_LIMIT ((atomic_val_t)CONFIG_AKIRA_WASM_APP_MEM_LIMIT_KB * 1024)

static atomic_t g_mem_limit_warned; /* one warning per slot per run */

static int wamr_hdr_slot(uint64_t hdr)
{
    return (int)((hdr >> (32 + WAMR_HDR_SLOT_SHIFT)) & 0xFFu) - 1;
}

static bool wamr_charge(int slot, uint32_t n)
{
    atomic_t *used = &g_apps[slot].wamr_bytes;

    if ((atomic_val_t)n > WAMR_APP_MEM_LIMIT ||
        atomic_add(used, (atomic_val_t)n) + (atomic_val_t)n > WAMR_APP_MEM_LIMIT)
    {
        if ((atomic_val_t)n <= WAMR_APP_MEM_LIMIT)
        {
            atomic_sub(used, (atomic_val_t)n);
        }
        if (!atomic_test_and_set_bit(&g_mem_limit_warned, slot))
        {
            LOG_WRN("App '%s' (slot %d) hit its %d KB memory limit; allocation refused",
                    g_apps[slot].name, slot, CONFIG_AKIRA_WASM_APP_MEM_LIMIT_KB);
        }
        return false;
    }
    return true;
}

static void wamr_uncharge(int slot, uint32_t n)
{
    atomic_t *used = &g_apps[slot].wamr_bytes;
    atomic_val_t old;
    atomic_val_t now;

    do
    {
        old = atomic_get(used);
        now = old > (atomic_val_t)n ? old - (atomic_val_t)n : 0;
    } while (!atomic_cas(used, old, now));
}

static void *wamr_alloc_raw(unsigned int size, int slot)
{
    /* Request enough space for padding (up to 7 bytes) + header + data */
    size_t total = sizeof(uint64_t) + 7 + (size_t)size;
    uint8_t *raw = slot >= 0 ? akira_malloc_app_buffer(total) : akira_malloc_buffer(total);

    if (!raw)
    {
        return NULL;
    }
    /* Round raw up to next 8-byte boundary */
    uintptr_t hdr_addr = ((uintptr_t)raw + 7) & ~(uintptr_t)7;
    uint32_t padding = (uint32_t)(hdr_addr - (uintptr_t)raw);
    uint32_t tag = slot >= 0 ? (uint32_t)(slot + 1) << WAMR_HDR_SLOT_SHIFT : 0;
    uint64_t *hdr = (uint64_t *)hdr_addr;

    *hdr = ((uint64_t)(padding | tag) << 32) | (uint64_t)size;
    return hdr + 1; /* user ptr is 8-byte aligned */
}

static void wamr_free_raw(void *ptr)
{
    uint64_t *hdr = (uint64_t *)ptr - 1;
    uint32_t padding = (uint32_t)(*hdr >> 32) & 0xFFu;

    akira_free_buffer((uint8_t *)hdr - padding);
}

static void *wamr_malloc(unsigned int size)
{
    int slot = akira_runtime_slot_of_thread(k_current_get());

    if (slot >= 0 && !wamr_charge(slot, size))
    {
        return NULL;
    }
    void *ptr = wamr_alloc_raw(size, slot);

    if (!ptr && slot >= 0)
    {
        wamr_uncharge(slot, size);
    }
    return ptr;
}

static void wamr_free(void *ptr)
{
    if (!ptr)
    {
        return;
    }
    uint64_t hdr = *((uint64_t *)ptr - 1);
    int slot = wamr_hdr_slot(hdr);

    if (slot >= 0 && slot < AKIRA_MAX_WASM_INSTANCES)
    {
        wamr_uncharge(slot, (uint32_t)hdr);
    }
    wamr_free_raw(ptr);
}

static void *wamr_realloc(void *ptr, unsigned int new_size)
{
    if (!ptr)
    {
        return wamr_malloc(new_size);
    }
    if (new_size == 0)
    {
        wamr_free(ptr);
        return NULL;
    }

    uint64_t hdr = *((uint64_t *)ptr - 1);
    uint32_t old_size = (uint32_t)hdr;
    int old_slot = wamr_hdr_slot(hdr);
    int slot = old_slot >= 0 ? old_slot : akira_runtime_slot_of_thread(k_current_get());
    /* Charge only the growth: the old block goes right after the copy. */
    uint32_t credit = (old_slot >= 0 && old_slot == slot) ? old_size : 0;
    uint32_t grow = new_size > credit ? new_size - credit : 0;

    if (slot >= 0 && grow && !wamr_charge(slot, grow))
    {
        return NULL;
    }
    void *new_ptr = wamr_alloc_raw(new_size, slot);

    if (!new_ptr)
    {
        if (slot >= 0 && grow)
        {
            wamr_uncharge(slot, grow);
        }
        return NULL;
    }

    size_t copy_size = (size_t)(old_size < new_size ? old_size : new_size);
    memcpy(new_ptr, ptr, copy_size);
    if (old_slot >= 0 && old_slot != slot)
    {
        wamr_uncharge(old_slot, old_size);
    }
    wamr_free_raw(ptr);
    if (slot >= 0 && credit > new_size)
    {
        wamr_uncharge(slot, credit - new_size);
    }
    return new_ptr;
}
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* Capability checking is implemented in src/runtime/security.c */

/* Helper: find by id validity */
static bool slot_valid(int id)
{
    return (id >= 0 && id < AKIRA_MAX_WASM_INSTANCES && g_apps[id].used);
}

/* O(1) cap lookup — slot pointer is stored as WAMR custom data at instantiation */
uint64_t akira_runtime_get_cap_mask_for_module_inst(wasm_module_inst_t inst)
{
    if (!inst)
    {
        return 0;
    }

    /* Fast path: custom data set in wasm_app_thread_fn */
    akira_managed_app_t *app =
        (akira_managed_app_t *)wasm_runtime_get_custom_data(inst);
    if (app)
    {
        return app->cap_mask;
    }

    /* Fallback: instance map (shouldn't normally reach here) */
    int slot = instance_map_get(inst);
    if (slot >= 0)
    {
        return g_apps[slot].cap_mask;
    }
    return 0;
}

int akira_runtime_get_name_for_module_inst(wasm_module_inst_t inst, char *buf, size_t buflen)
{
    if (!inst || !buf || buflen == 0)
        return -EINVAL;
    /* No lock: natives call this from app threads, and a thread aborted while
     * holding g_runtime_mutex would block every later load. The slot is found
     * through the instance's own custom data and checked against it; a live
     * instance's slot can't be reused under it. */
#ifdef CONFIG_AKIRA_WASM_RUNTIME
    akira_managed_app_t *app = (akira_managed_app_t *)wasm_runtime_get_custom_data(inst);

    if (app >= g_apps && app < g_apps + AKIRA_MAX_WASM_INSTANCES &&
        app->used && app->instance == inst)
    {
        strncpy(buf, app->name, buflen - 1);
        buf[buflen - 1] = '\0';
        return 0;
    }
#endif
    return -ENOENT;
}

/* Get app slot from module instance - uses O(1) hash map with linear fallback */
int get_slot_for_module_inst(wasm_module_inst_t inst)
{
    if (!inst)
        return -1;

    /* Fast path: O(1) hash map lookup */
    int slot = instance_map_get(inst);
    if (slot >= 0)
        return slot;

    /* Slow path fallback: linear scan (shouldn't normally reach here) */
    for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++)
    {
        if (g_apps[i].used && g_apps[i].instance == inst)
        {
            /* Repair the map for next lookup */
            instance_map_put(inst, i);
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
    if (!slot_valid(instance_id))
    {
        return 0;
    }
    return (uint32_t)atomic_get(&g_apps[instance_id].memory_used);
}

/**
 * @brief Get memory quota for an app
 *
 * @param instance_id  App instance ID
 * @return Memory quota in bytes (0 = unlimited), or 0 if invalid
 */
uint32_t akira_runtime_get_memory_quota(int instance_id)
{
    if (!slot_valid(instance_id))
    {
        return 0;
    }
    return g_apps[instance_id].memory_quota;
}

uint64_t akira_runtime_get_cap_mask(int instance_id)
{
    if (!slot_valid(instance_id))
    {
        return 0;
    }
    return g_apps[instance_id].cap_mask;
}

int akira_runtime_get_commands_json(int instance_id, char *buf, size_t buf_len)
{
    if (!slot_valid(instance_id) || !buf || buf_len == 0)
    {
        return -EINVAL;
    }
    strncpy(buf, g_apps[instance_id].commands_json, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return 0;
}

/* Initialize WAMR runtime with PSRAM-backed heap when available */
int akira_runtime_init(void)
{
    /* Atomic compare-and-swap: first caller wins, subsequent calls are no-ops. */
    if (!atomic_cas(&g_runtime_initialized, 0, 1))
    {
        return 0;
    }

    LOG_INF("Initializing Akira unified runtime v2...");

    /* Refuse to run apps against a capability registry with colliding names or
     * bits: capability checks would silently grant or deny the wrong thing. */
    int registry_ret = akira_capability_registry_validate();
    if (registry_ret < 0)
    {
        LOG_ERR("Capability registry is invalid (%d); see errors above", registry_ret);
        atomic_set(&g_runtime_initialized, 0);
        return registry_ret;
    }

    /* Initialize performance & security subsystems */
    module_cache_init();
    instance_map_init();
    sandbox_init();
    app_signing_init();

    /* WASM app thread stacks were pre-allocated by wasm_stacks_early_alloc()
     * SYS_INIT hook (APPLICATION, 1) before BT/WiFi/USB could fragment the
     * heap. Just initialize the per-slot deferred cleanup work items here. */
    for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++)
    {
        g_slot_cleanup[i].slot = i;
        k_work_init(&g_slot_cleanup[i].work, slot_cleanup_work_fn);
        /* Once, never per run: a stop may still be waiting on them while the
         * slot is reused. */
        k_mutex_init(&g_apps[i].exit_mutex);
        k_condvar_init(&g_apps[i].cond_exit);
        if (!g_app_stacks[i])
        {
            LOG_ERR("SRAM stack for slot %d missing (early alloc failed)", i);
        }
    }
    LOG_DBG("App thread stacks: %u B each, pre-allocated at boot",
            CONFIG_AKIRA_WASM_APP_STACK_SIZE);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    /*
     * Initialize WAMR with Alloc_With_Allocator.
     *
     * This replaces the old static Alloc_With_Pool approach which reserved
     * CONFIG_WAMR_HEAP_SIZE (256 KB default) as BSS on RAM-only targets.
     * With Alloc_With_Allocator, WAMR calls our trampolines which route to
     * akira_malloc_buffer() — PSRAM-first on ESP32-S3, system heap on nRF54L15.
     * Memory is only consumed when WASM apps actually allocate it.
     */
    RuntimeInitArgs init_args = {0};
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func = wamr_malloc;
    init_args.mem_alloc_option.allocator.free_func = wamr_free;
    init_args.mem_alloc_option.allocator.realloc_func = wamr_realloc;

    if (!wasm_runtime_full_init(&init_args))
    {
        LOG_ERR("WAMR runtime initialization failed");
        return -ENODEV;
    }

#ifdef CONFIG_AKIRA_WASM_API
    /* Register every AKIRA_NATIVE_API_DEFINE() table ("env", "akira_log",
     * "akira_time", product natives) with WAMR. A native defined twice stops
     * the runtime: WAMR would silently let one definition shadow the other. */
    int natives_ret = akira_native_registry_register_all();
    if (natives_ret < 0)
    {
        LOG_ERR("Failed to register native APIs (%d)", natives_ret);
        return natives_ret;
    }
#else
    LOG_WRN("Native API registration not included - no APIs enabled (CONFIG_AKIRA_WASM_API not set)");
#endif /* CONFIG_AKIRA_WASM_API */

    /* Ensure WASM apps dir exists */
    if (fs_manager_exists("/lfs/apps") != 1)
    { // Not found
        fs_manager_mkdir("/lfs/apps");
    }

    /* g_runtime_initialized already set to 1 by the atomic_cas above */
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
static int load_wasm_ex(const uint8_t *buffer, uint32_t size,
                        const char *manifest_json, size_t manifest_len,
                        const char *app_name,
                        const struct akira_admission_record *record)
{
    if (!atomic_get(&g_runtime_initialized))
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (!buffer || size < 8 ||
        (memcmp(buffer, "\0asm", 4) != 0 && memcmp(buffer, "\0aot", 4) != 0))
    {
        LOG_ERR("Invalid WASM/AOT binary");
        return -EINVAL;
    }

    /* Atomically find and reserve a slot under g_runtime_mutex so two
     * concurrent load_wasm() calls cannot pick the same free slot. */
    int slot = -ENOMEM;
    k_mutex_lock(&g_runtime_mutex, K_FOREVER);
    for (int _i = 0; _i < AKIRA_MAX_WASM_INSTANCES; _i++)
    {
        if (!g_apps[_i].used)
        {
            slot = _i;
            g_apps[_i].used = true; /* reserve immediately under lock */
            g_apps[_i].generation++;
            break;
        }
    }
    k_mutex_unlock(&g_runtime_mutex);
    if (slot < 0)
    {
        LOG_ERR("No free slots for WASM modules");
        return slot;
    }

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    /* ===== Step 1: Integrity verification ===== */
    uint8_t binary_hash[32];
    int integrity_ret = app_verify_wasm_integrity(buffer, size, binary_hash);
    if (integrity_ret != 0)
    {
        LOG_ERR("WASM binary integrity check failed: %d", integrity_ret);
        sandbox_audit_log(AUDIT_EVENT_INTEGRITY_FAIL, "load", (uint32_t)size);
        g_apps[slot].used = false; /* release reserved slot */
        return integrity_ret;
    }

    /* ===== Step 2: Parse manifest =====
     * The manifest embedded in the binary wins. A JSON manifest supplied next to
     * the binary (sidecar file, package) is used only when the binary has none;
     * the two are never merged. */
    int64_t load_start_ms = k_uptime_get();

    akira_manifest_t manifest;
    bool manifest_from_binary = false;
    manifest_init_defaults(&manifest);
    if (manifest_parse_wasm_section(buffer, size, &manifest) == 0)
    {
        manifest_from_binary = true;
        LOG_INF("Found embedded manifest: cap_mask=0x%016llx, memory_quota=%u",
                (unsigned long long)manifest.cap_mask, manifest.memory_quota);
    }
    else if (manifest_json && manifest_len > 0)
    {
        manifest_init_defaults(&manifest);
        if (manifest_parse_json(manifest_json, manifest_len, &manifest) == 0)
        {
            LOG_INF("Using supplied manifest: cap_mask=0x%016llx, memory_quota=%u",
                    (unsigned long long)manifest.cap_mask, manifest.memory_quota);
        }
    }

    /* ===== Step 3: ABI / minimum-version gate =====
     * The choke point every install and load path reaches. app_manager also gates
     * before writing to flash, but SD-XIP, cloud and streaming loads come here
     * directly, so re-check to refuse an app built for an incompatible ABI or a
     * newer AkiraOS (also catches an app installed before a firmware downgrade). */
    int abi_ret = akira_abi_check_default(&manifest);
    if (abi_ret < 0)
    {
        g_apps[slot].used = false; /* release reserved slot */
        return abi_ret;
    }

    /* ===== Step 3b: Admission (akira_admission.h) =====
     * Every load path reaches here. The record written at install binds the
     * app to its binary and to the manifest JSON stored next to it; a load
     * whose bytes differ from what was admitted is refused outright. */
    const char *adm_name = app_name ? app_name :
                           (manifest.valid && manifest.name[0] ? manifest.name : "");
    bool json_in_use = !manifest_from_binary && manifest_json && manifest_len > 0;
    uint8_t manifest_hash[32];

    if (json_in_use &&
        app_compute_hash(manifest_json, manifest_len, manifest_hash) != 0)
    {
        g_apps[slot].used = false;
        return -EIO;
    }
    if (record)
    {
        /* No JSON at install leaves manifest_sha256 all-zero, so a sidecar
         * JSON added later never matches. */
        bool bin_ok = memcmp(record->binary_sha256, binary_hash, 32) == 0;
        bool mf_ok = !json_in_use ||
                     memcmp(record->manifest_sha256, manifest_hash, 32) == 0;
        if (!bin_ok || !mf_ok)
        {
            LOG_ERR("'%s' does not match what was admitted at install (%s changed)",
                    adm_name, bin_ok ? "manifest" : "binary");
            sandbox_audit_log(AUDIT_EVENT_INTEGRITY_FAIL, adm_name, bin_ok ? 2 : 1);
            g_apps[slot].used = false;
            return -EACCES;
        }
    }

    uint64_t requested_mask = manifest.valid ? manifest.cap_mask : 0;
    struct akira_admission_ctx adm_ctx = {
        .phase = AKIRA_ADMISSION_LOAD,
        .source = record ? (enum akira_app_source)record->source
                         : AKIRA_APP_SOURCE_UNKNOWN,
        .name = adm_name,
        .version = manifest.valid ? manifest.version : "",
        .requested_caps = requested_mask,
        .binary_sha256 = binary_hash,
        .manifest_sha256 = json_in_use ? manifest_hash : NULL,
        .manifest_embedded = manifest_from_binary,
        .net_policy = manifest.valid ? &manifest.net_policy : NULL,
        .record = record,
    };
    struct akira_admission_result adm;

    if (akira_admission_evaluate(&adm_ctx, &adm) != 0)
    {
        g_apps[slot].used = false;
        return -EACCES;
    }

    /* An AOT file is native code; WAMR's bounds checks only confine it if
     * wamrc produced it, which the loader cannot tell. Running an unattested
     * one is the user's choice unless the build refuses it. */
    if (!adm.attested && memcmp(buffer, "\0aot", 4) == 0)
    {
        if (IS_ENABLED(CONFIG_AKIRA_REFUSE_UNATTESTED_AOT))
        {
            LOG_ERR("AOT binary refused: '%s' is not attested", adm_name);
            sandbox_audit_log(AUDIT_EVENT_INTEGRITY_FAIL, "aot_unattested", size);
            g_apps[slot].used = false; /* release reserved slot */
            return -EPERM;
        }
        LOG_WRN("'%s' is unattested AOT native code: the sandbox only confines "
                "it if wamrc built it from a source you trust", adm_name);
        sandbox_audit_log(AUDIT_EVENT_APP_LOADED, "aot_unattested", size);
    }

    /* ===== Step 4: Load WASM module ===== */
    /* Allocate a small probe buffer to check if PSRAM is available,
     * then free it immediately — we only need the source type (chunk_src). */
    mem_source_t chunk_src;
    uint8_t *chunk_probe = akira_malloc_buffer_ex(CHUNK_BUFFER_SIZE, &chunk_src);
    if (!chunk_probe)
    {
        LOG_ERR("Failed to probe memory source (%d bytes)", CHUNK_BUFFER_SIZE);
        g_apps[slot].used = false; /* release reserved slot */
        return -ENOMEM;
    }
    LOG_INF("Memory source: %s",
            chunk_src == MEM_SOURCE_PSRAM ? "PSRAM" : "SRAM");
    akira_free_buffer(chunk_probe);
    chunk_probe = NULL;

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

            /* Copy directly from source to PSRAM in chunks.
             * No intermediate bounce buffer needed — source is a contiguous
             * SRAM/flash region, and chunking here only serves to preserve
             * cache/DMA locality on the PSRAM write path. */
            uint32_t offset = 0;
            while (offset < size)
            {
                uint32_t chunk_len = MIN(CHUNK_BUFFER_SIZE, size - offset);
                memcpy(staged_buffer + offset, buffer + offset, chunk_len);
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

    /* WAMR stores raw pointers into the binary buffer for export/import name
     * strings when called via wasm_runtime_load() (which sets
     * wasm_binary_freeable=false, causing reuse_const_strings=true in
     * load_from_sections). The buffer MUST outlive wasm_runtime_unload() or
     * every wasm_runtime_lookup_function() call will hit dangling pointers.
     *
     * Strategy: reuse staged_buffer if we already copied to PSRAM; otherwise
     * allocate a fresh owned copy. The caller's `buffer` is never borrowed. */
    uint8_t *owned_binary;
    if (staged_buffer)
    {
        /* Transfer ownership — staged_buffer is already a full PSRAM copy */
        owned_binary = staged_buffer;
        staged_buffer = NULL;
    }
    else
    {
        /* load_buffer == buffer (caller-owned) — make our own durable copy */
        owned_binary = akira_malloc_buffer(size);
        if (!owned_binary)
        {
            LOG_ERR("OOM: cannot allocate owned WASM binary copy (%u bytes)", size);
            g_apps[slot].used = false; /* release reserved slot */
            return -ENOMEM;
        }
        memcpy(owned_binary, buffer, size);
    }

    /* Load the WASM module — owned_binary is intentionally NOT freed here */
    module = wasm_runtime_load(owned_binary, size, error_buf, sizeof(error_buf));

    if (!module)
    {
        akira_free_buffer(owned_binary);
        LOG_ERR("wasm_runtime_load failed: %s", error_buf);
        g_apps[slot].used = false; /* release reserved slot */
        return -EIO;
    }

    /* Measure load time for profiling */
    uint32_t load_time_ms = (uint32_t)(k_uptime_get() - load_start_ms);

    /* g_apps[slot].used was already set true during slot reservation */
    g_apps[slot].module = module;
    g_apps[slot].wasm_binary = owned_binary; /* kept alive until wasm_runtime_unload */
    g_apps[slot].status = AKIRA_APP_STATUS_CREATED;
    g_apps[slot].exit_code = 0;
    g_apps[slot].tid = NULL;
    /* Grant capabilities once, here. An unattested app's request is clamped
     * (CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK, CONFIG_AKIRA_HARDEN_UNATTESTED_APPS);
     * admission clamps (install record + this load) apply to every app. Never
     * grant the raw manifest mask verbatim — an app must not self-assert "*". */
    uint64_t unattested_privileged = 0;
    g_apps[slot].cap_mask = akira_capability_grant(requested_mask, adm.attested,
                                                   &unattested_privileged) &
                            adm.cap_clamp &
                            (record ? record->cap_clamp : UINT64_MAX);
    g_apps[slot].attested = adm.attested;

    if (unattested_privileged) {
        /* An unsigned app was granted world-affecting capabilities. This is
         * allowed (default dev posture) but must be visible in the audit log. */
        LOG_WRN("Unattested app granted privileged caps: 0x%016llx",
                (unsigned long long)unattested_privileged);
        sandbox_audit_log(AUDIT_EVENT_SIGNATURE_FAIL, "unsigned_privileged",
                          (uint32_t)(g_apps[slot].cap_mask & 0xFFFFFFFFu));
    }
    if (g_apps[slot].cap_mask != requested_mask) {
        LOG_INF("Capability mask sanitized: requested=0x%016llx granted=0x%016llx (attested=%d)",
                (unsigned long long)requested_mask,
                (unsigned long long)g_apps[slot].cap_mask, adm.attested);
    }
    g_apps[slot].memory_quota = manifest.valid ? manifest.memory_quota : 0;
    strncpy(g_apps[slot].commands_json, manifest.commands_json,
            sizeof(g_apps[slot].commands_json) - 1);
    g_apps[slot].commands_json[sizeof(g_apps[slot].commands_json) - 1] = '\0';
    atomic_set(&g_apps[slot].memory_used, 0);
    memcpy(g_apps[slot].binary_hash, binary_hash, 32);
    g_apps[slot].hash_valid = true;
    g_apps[slot].trust_level = TRUST_LEVEL_USER;
    snprintf(g_apps[slot].name, sizeof(g_apps[slot].name),
             manifest.valid && manifest.name[0] ? manifest.name : "app%d", slot);

    /* Initialize sandbox and performance tracking */
    sandbox_ctx_init(&g_apps[slot].sandbox, TRUST_LEVEL_USER, g_apps[slot].cap_mask);
    memset(&g_apps[slot].perf, 0, sizeof(runtime_perf_stats_t));

    /* Store in module cache for future reuse */
    module_cache_store(binary_hash, module, size, load_time_ms);

    sandbox_audit_log(AUDIT_EVENT_APP_LOADED, g_apps[slot].name, (uint32_t)size);
    LOG_INF("WASM module loaded into slot %d (cap=0x%016llx, quota=%u, load=%ums)",
            slot, (unsigned long long)g_apps[slot].cap_mask, g_apps[slot].memory_quota, load_time_ms);
    return slot;
#else
    (void)buffer;
    (void)size;
    (void)manifest_json;
    (void)manifest_len;
    return -ENOTSUP; /* WASM runtime not available */
#endif
}

int akira_runtime_load_wasm(const uint8_t *buffer, uint32_t size)
{
    return load_wasm_ex(buffer, size, NULL, 0, NULL, NULL);
}

/* ===== Thread-per-app implementation ===== */

#ifdef CONFIG_AKIRA_WASM_RUNTIME

/**
 * @brief WASM app thread entry point.
 *
 * Each WASM app runs in its own Zephyr thread so the shell and other
 * subsystems remain responsive while apps execute.  Flow:
 *   1. Instantiate WAMR module inside the thread (safe for all WAMR modes)
 *   2. Post sem_start so akira_runtime_start() can return to the caller
 *   3. Run wasm_runtime_call_wasm() — blocks until app exits or is terminated
 *   4. Clean up WAMR resources and signal cond_exit for any waiters
 */
static void wasm_app_thread_fn(void *p1, void *p2, void *p3)
{
    int slot = (int)(intptr_t)p1;
    (void)p2;
    (void)p3;

    akira_managed_app_t *app = &g_apps[slot];

#ifndef CONFIG_WAMR_INSTANCE_HEAP
#define CONFIG_WAMR_INSTANCE_HEAP 65536
#endif
#ifndef CONFIG_WAMR_STACK_SIZE
#define CONFIG_WAMR_STACK_SIZE 8192
#endif

    /* ------ Step 1: Instantiate ------ */
    char error_buf[128] = {0};
    wasm_module_inst_t inst = wasm_runtime_instantiate(
        app->module,
        CONFIG_WAMR_INSTANCE_HEAP,
        CONFIG_WAMR_STACK_SIZE,
        error_buf, sizeof(error_buf));

    if (!inst)
    {
        LOG_ERR("wasm_runtime_instantiate failed (slot %d): %s", slot, error_buf);
        app->exit_code = -EIO;
        app->status = AKIRA_APP_STATUS_ERROR;
        k_sem_give(&app->sem_start);
        goto thread_exit;
    }

    /* Use the singleton exec_env — one exec_env per instance is the correct
     * WAMR embedded model. Creating a second exec_env with
     * wasm_runtime_create_exec_env() corrupts the instance's export_functions
     * table in Alloc_With_Allocator mode, causing wasm_runtime_lookup_function
     * to return NULL even when exports are present in the binary. */
    wasm_exec_env_t exec_env = wasm_runtime_get_exec_env_singleton(inst);
    if (!exec_env)
    {
        LOG_ERR("Failed to get singleton exec_env for slot %d", slot);
        wasm_runtime_deinstantiate(inst);
        app->exit_code = -ENOMEM;
        app->status = AKIRA_APP_STATUS_ERROR;
        k_sem_give(&app->sem_start);
        goto thread_exit;
    }

    app->instance = inst;
    app->exec_env = exec_env; /* singleton — do NOT call destroy_exec_env on it */

    /* O(1) cap lookup: store slot pointer as WAMR custom data */
    wasm_runtime_set_custom_data(inst, &g_apps[slot]);
    instance_map_put(inst, slot);

    wasm_runtime_clear_exception(inst);

    /* ------ Step 2: Signal caller that instance is ready ------ */
    k_sem_give(&app->sem_start);

    /* ------ Step 3: Execute ------ */
    sandbox_exec_begin(&app->sandbox);
    perf_exec_begin(&app->perf);

    /* wasm_application_execute_main() is WAMR's built-in entry-point runner.
     * It tries "main", "__main_argc_argv", "_main", and "_start" (WASI) in
     * order, handles argc/argv marshalling, and uses the singleton exec_env
     * so all native API calls receive the right context for cap checking. */
    int exit_code = 0;
    bool ran = wasm_application_execute_main(inst, 0, NULL);
    if (!ran)
    {
        const char *ex = wasm_runtime_get_exception(inst);
        if (ex && strstr(ex, "lookup the entry point"))
        {
            /* No main/start entry point — treat as a reactive (library) module */
            LOG_INF("Slot %d: no entry point — reactive module", slot);
            wasm_runtime_clear_exception(inst);
        }
        else if (ex && app->stop_requested)
        {
            /* akira_runtime_stop() asked WAMR to unwind this app. WAMR reports
             * that as the exception "terminated by user": an orderly exit,
             * not a trap. */
            LOG_INF("Slot %d stopped on request (%s)", slot, ex);
            wasm_runtime_clear_exception(inst);
        }
        else if (ex)
        {
            LOG_ERR("WASM exception (slot %d): %s", slot, ex);
            app->perf.trap_count++;
            exit_code = -EIO;
        }
    }

    perf_exec_end(&app->perf);

    /* ------ Step 4: Release what the app left running ------
     * Before deinstantiate: anything holding pointers into the app's linear
     * memory must stop using them first. From here on this thread runs only
     * host code that takes system and WAMR locks, so a concurrent stop must
     * wait for it rather than abort it (reset when the slot next starts). */
    atomic_inc(&app->native_critical);
    akira_app_resources_release(slot, app->name);

    /* ------ Step 4b: Clean up WAMR resources ------ */
    /* Singleton exec_env is automatically freed by wasm_runtime_deinstantiate;
     * never call wasm_runtime_destroy_exec_env on it. */
    app->exec_env = NULL;

    AKIRA_DISPLAY_RELEASE_OWNER(inst);
    /* Clear the slot's pointer first: a fault inside deinstantiate must not
     * make the crash cleanup free it a second time. */
    app->instance = NULL;
    instance_map_remove(inst);
    wasm_runtime_deinstantiate(inst);

    atomic_set(&app->memory_used, 0);
    sandbox_exec_end(&app->sandbox);
    sandbox_audit_log(AUDIT_EVENT_APP_STOPPED, app->name, (uint32_t)slot);

    app->exit_code = (int8_t)exit_code;
    LOG_INF("App thread done (slot %d, exit=%d, calls=%u, traps=%u)",
            slot, exit_code, app->perf.call_count, app->perf.trap_count);

thread_exit:
    /* Notify any waiters (akira_runtime_stop or status query).
     * Use the per-slot exit_mutex — fully isolates concurrent stop() calls
     * on different slots (no false contention across unrelated apps). */
    k_mutex_lock(&app->exit_mutex, K_FOREVER);
    if (app->status != AKIRA_APP_STATUS_ERROR)
    {
        app->status = AKIRA_APP_STATUS_EXITED;
    }
    k_condvar_broadcast(&app->cond_exit);
    k_mutex_unlock(&app->exit_mutex);

    /* -------- Release the WASM module and slot --------
     * Done here (in the app thread itself) to avoid a k_thread_join deadlock
     * if the exit callback tried to call akira_runtime_destroy().
     * exec_env and instance are already NULL (cleaned up in Step 4 above).
     * Only the module handle and the slot itself remain to free. */
    /* Each pointer is cleared before its free, as in reclaim_dead_slot(). */
    if (app->module)
    {
        wasm_module_t mod = app->module;

        app->module = NULL;
        wasm_runtime_unload(mod);
    }
    /* Free the owned binary copy now that the module is unloaded.
     * Must come AFTER wasm_runtime_unload() — WAMR holds raw pointers
     * into this buffer for export/import name strings. */
    if (app->wasm_binary)
    {
        uint8_t *bin = app->wasm_binary;

        app->wasm_binary = NULL;
        akira_free_buffer(bin);
    }
    if (app->hash_valid)
    {
        module_cache_release(app->binary_hash);
        module_cache_invalidate(app->binary_hash);
        app->hash_valid = false;
    }
    app->name[0] = '\0';
    app->tid = NULL;
    app->used = false; /* Slot now free — must be LAST write */

    /* Invoke registered exit callback (e.g. app_manager state transition).
     * Read g_exit_cb under g_runtime_mutex to guard against a concurrent
     * set_exit_callback() tearing the pointer on 64-bit targets (native_sim). */
    akira_runtime_exit_cb_t cb = get_exit_cb();
    if (cb)
    {
        cb(slot, app->exit_code);
    }

    /* Schedule deferred stack cleanup on the system work queue.
     * We cannot free g_app_stacks[slot] here — we're still executing on it.
     * The work handler (slot_cleanup_work_fn) runs in sysworkq context,
     * calls k_thread_join() to confirm the thread is dead, then frees the
     * 8 KB SRAM heap stack so it can be reused for the next app launch. */
    k_work_submit(&g_slot_cleanup[slot].work);
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* ===== Containment: no app can hang or crash the system =====
 * See include/akira_app_containment.h and docs/architecture/app-containment.md. */

int akira_runtime_slot_of_thread(k_tid_t tid)
{
    for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++)
    {
        if (tid == &g_app_threads[i] && g_apps[i].used)
        {
            return i;
        }
    }
    return -1;
}

static akira_runtime_exit_cb_t get_exit_cb(void)
{
    akira_runtime_exit_cb_t cb;

    k_mutex_lock(&g_runtime_mutex, K_FOREVER);
    cb = g_exit_cb;
    k_mutex_unlock(&g_runtime_mutex);
    return cb;
}

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* Free what a dead (aborted or faulted) app thread never got to free. Every
 * pointer is cleared before its free, so a second caller is harmless. Caller
 * holds app->exit_mutex. */
static void reclaim_dead_slot(int slot, int exit_code)
{
    akira_managed_app_t *app = &g_apps[slot];

    /* First: nothing may keep using the app's memory once it is freed. */
    akira_app_resources_release(slot, app->name);
    app->exec_env = NULL; /* the singleton goes with the instance */
    if (app->instance)
    {
        wasm_module_inst_t inst = app->instance;

        app->instance = NULL;
        AKIRA_DISPLAY_RELEASE_OWNER(inst);
        instance_map_remove(inst);
        wasm_runtime_deinstantiate(inst);
    }
    if (app->module)
    {
        wasm_module_t mod = app->module;

        app->module = NULL;
        wasm_runtime_unload(mod);
    }
    if (app->wasm_binary)
    {
        uint8_t *bin = app->wasm_binary;

        app->wasm_binary = NULL;
        akira_free_buffer(bin);
    }
    if (app->hash_valid)
    {
        app->hash_valid = false;
        module_cache_release(app->binary_hash);
        module_cache_invalidate(app->binary_hash);
    }
    atomic_set(&app->memory_used, 0);
    /* Not sandbox_exec_end(): that acts on the calling thread, and this runs
     * on the stopping thread or the system workqueue. */
    app->sandbox.exec_active = false;
    app->exit_code = (int8_t)exit_code;
}

/* After reclaim_dead_slot(): give the slot back and tell the app manager,
 * exactly as a thread that exits on its own does. */
static void free_dead_slot(int slot, int exit_code)
{
    akira_managed_app_t *app = &g_apps[slot];
    akira_runtime_exit_cb_t cb;

    app->name[0] = '\0';
    app->tid = NULL;
    app->used = false; /* must be the last write */

    cb = get_exit_cb();
    if (cb)
    {
        cb(slot, exit_code);
    }
}

static akira_managed_app_t *app_of_env(wasm_exec_env_t exec_env)
{
    wasm_module_inst_t inst = exec_env ? wasm_runtime_get_module_inst(exec_env) : NULL;

    return inst ? (akira_managed_app_t *)wasm_runtime_get_custom_data(inst) : NULL;
}

int akira_runtime_slot_of_exec_env(wasm_exec_env_t exec_env)
{
    akira_managed_app_t *app = app_of_env(exec_env);

    return app ? (int)(app - g_apps) : -1;
}

bool akira_runtime_stop_requested(wasm_exec_env_t exec_env)
{
    akira_managed_app_t *app = app_of_env(exec_env);

    return app && app->stop_requested;
}

void akira_native_critical_enter(wasm_exec_env_t exec_env)
{
    akira_managed_app_t *app = app_of_env(exec_env);

    if (app)
    {
        atomic_inc(&app->native_critical);
    }
}

void akira_native_critical_exit(wasm_exec_env_t exec_env)
{
    akira_managed_app_t *app = app_of_env(exec_env);

    if (app && atomic_get(&app->native_critical) > 0)
    {
        atomic_dec(&app->native_critical);
    }
}

bool akira_runtime_sleep(wasm_exec_env_t exec_env, k_timeout_t timeout)
{
    akira_managed_app_t *app = app_of_env(exec_env);

    if (!app)
    {
        k_sleep(timeout);
        return false;
    }
    if (app->stop_requested)
    {
        return true;
    }
    if (k_sem_take(&app->stop_sem, timeout) == 0)
    {
        k_sem_give(&app->stop_sem); /* stay signalled for later waits */
        return true;
    }
    return false;
}

/* ---- a fault in an app thread kills the app, not the system ------------- */

static atomic_t g_fault_slots;
static uint8_t g_fault_reason[AKIRA_MAX_WASM_INSTANCES];
static bool g_fault_in_critical[AKIRA_MAX_WASM_INSTANCES];

static void fault_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    for (int slot = 0; slot < AKIRA_MAX_WASM_INSTANCES; slot++)
    {
        if (!atomic_test_and_clear_bit(&g_fault_slots, slot))
        {
            continue;
        }
        akira_managed_app_t *app = &g_apps[slot];
        akira_runtime_exit_cb_t cb;

        /* Zephyr aborted the thread when the fatal handler returned. */
        k_thread_join(&g_app_threads[slot], K_MSEC(200));

        k_mutex_lock(&app->exit_mutex, K_FOREVER);
        /* Whatever state the thread reached (running, or already in its exit
         * path), a crashed thread never frees its slot itself. */
        if (!app->used || !app->crashed || app->parked)
        {
            k_mutex_unlock(&app->exit_mutex);
            continue;
        }
        if (g_fault_in_critical[slot])
        {
            /* It died holding a system lock (a bus, the filesystem, the heap
             * of a driver). The release hooks could block on that lock
             * forever, and wedge this work queue with them, so they don't
             * run: the slot keeps its memory and resources and is never
             * reused, so nothing else can reach what it owned. */
            LOG_ERR("App '%s' (slot %d) crashed (fatal error %u) inside a native holding "
                    "a system lock: stopped it; the slot stays parked until reboot",
                    app->name, slot, g_fault_reason[slot]);
            sandbox_audit_log(AUDIT_EVENT_APP_STOPPED, app->name, (uint32_t)slot);
            app->parked = true;
            app->tid = NULL;
            app->status = AKIRA_APP_STATUS_ERROR;
            k_condvar_broadcast(&app->cond_exit);
            k_mutex_unlock(&app->exit_mutex);

            cb = get_exit_cb();
            if (cb)
            {
                cb(slot, -EFAULT);
            }
            continue;
        }
        LOG_ERR("App '%s' (slot %d) crashed (fatal error %u): stopped it, system unaffected",
                app->name, slot, g_fault_reason[slot]);
        sandbox_audit_log(AUDIT_EVENT_APP_STOPPED, app->name, (uint32_t)slot);
        reclaim_dead_slot(slot, -EFAULT);
        app->status = AKIRA_APP_STATUS_ERROR;
        k_condvar_broadcast(&app->cond_exit);
        k_mutex_unlock(&app->exit_mutex);

        /* App manager: ERROR, restart policy, crash hook. */
        free_dead_slot(slot, -EFAULT);
    }
}

static K_WORK_DEFINE(g_fault_work, fault_work_fn);

/* True when the fault interrupted thread code, not an ISR or exception. */
static bool fault_in_thread_context(const struct arch_esf *esf)
{
#if defined(CONFIG_CPU_CORTEX_M)
    /* The fault handler itself runs as an exception, so k_is_in_isr() is
     * always true here. The stacked xPSR says what the fault interrupted:
     * exception number (IPSR) 0 is thread mode. */
    return esf != NULL && (esf->basic.xpsr & 0x1FFU) == 0U;
#elif defined(CONFIG_XTENSA)
    ARG_UNUSED(esf);
    /* Level-1 exceptions and interrupts share a vector; the exception entry
     * itself counts as one level of nesting. */
    return arch_curr_cpu()->nested <= 1;
#elif defined(CONFIG_ARCH_POSIX) || defined(CONFIG_RISCV)
    /* Faults don't count as interrupt nesting on these. */
    ARG_UNUSED(esf);
    return !k_is_in_isr();
#else
    /* Not verified for this architecture: keep Zephyr's policy. */
    ARG_UNUSED(esf);
    return false;
#endif
}
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

bool akira_runtime_contain_fault(unsigned int reason, const struct arch_esf *esf)
{
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_MULTITHREADING)
    int slot = akira_runtime_slot_of_thread(k_current_get());

    if (slot < 0 || reason == K_ERR_KERNEL_PANIC || !fault_in_thread_context(esf))
    {
        return false;
    }
#if !defined(CONFIG_HW_STACK_PROTECTION)
    /* Without a hardware stack guard, an overflow is found (by the sentinel
     * or a canary) only after it may have overwritten the heap memory next
     * to the app's stack, which belongs to someone else: keep Zephyr's
     * policy. WAMR keeps CONFIG_AKIRA_WASM_STACK_GUARD_SIZE free for natives,
     * so only a native using more than that can get here. */
    if (reason == K_ERR_STACK_CHK_FAIL)
    {
        return false;
    }
#endif
    /* The cleanup runs on the system work queue and logs from there: no
     * logging in fatal context. */
    g_apps[slot].crashed = true;
    g_fault_reason[slot] = (uint8_t)reason;
    g_fault_in_critical[slot] = atomic_get(&g_apps[slot].native_critical) > 0;
    atomic_set_bit(&g_fault_slots, slot);
    k_work_submit(&g_fault_work);
    return true;
#else
    ARG_UNUSED(reason);
    ARG_UNUSED(esf);
    return false;
#endif
}

/* Instantiate and run a loaded WASM module in a dedicated thread */
int akira_runtime_start(int instance_id)
{
    if (!slot_valid(instance_id))
    {
        return -EINVAL;
    }

    akira_managed_app_t *app = &g_apps[instance_id];

    if (app->parked)
    {
        return -EBUSY; /* see fault_work_fn() */
    }
    if (app->status == AKIRA_APP_STATUS_RUNNING)
    {
        return 0; /* already running */
    }

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    /* The previous thread on this slot must be gone before its k_thread and
     * stack are reused. app->tid is no proof: an exiting thread clears it and
     * then still runs the exit callback. Creating a thread on a live one
     * corrupts the scheduler, so refuse instead. */
    if (g_app_thread_created[instance_id])
    {
        int join_ret = k_thread_join(&g_app_threads[instance_id], K_MSEC(2000));

        if (join_ret != 0)
        {
            LOG_ERR("Slot %d: previous app thread still running (%d)", instance_id, join_ret);
            return -EBUSY;
        }
        app->tid = NULL;
    }

    /* Stack was pre-allocated at init — always available, no allocation needed.
     * Fail gracefully if init allocation failed (e.g. OOM at boot). */
    if (!g_app_stacks[instance_id])
    {
        LOG_ERR("SRAM stack for slot %d was never allocated (init failure)", instance_id);
        return -ENOMEM;
    }

    /* Per-run sync primitives. exit_mutex and cond_exit are initialised once
     * in akira_runtime_init(): a stop of the previous run may still wait on
     * them. */
    k_sem_init(&app->sem_start, 0, 1);
    app->generation++;
    app->exit_code = 0;
    app->stop_requested = false;
    app->crashed = false;
    k_sem_init(&app->stop_sem, 0, 1);
    atomic_set(&app->native_critical, 0);
    atomic_set(&app->wamr_bytes, 0);
    atomic_clear_bit(&g_mem_limit_warned, instance_id);

    /* RUNNING before the thread exists: an app can exit or crash before this
     * function gets the CPU back, and the exit path and the fault cleanup
     * act only on a RUNNING slot. Setting it afterwards turned a crash in
     * the first instructions into a zombie slot. */
    app->status = AKIRA_APP_STATUS_RUNNING;

    app->tid = k_thread_create(
        &g_app_threads[instance_id],
        g_app_stacks[instance_id],
        CONFIG_AKIRA_WASM_APP_STACK_SIZE,
        wasm_app_thread_fn,
        (void *)(intptr_t)instance_id, NULL, NULL,
        CONFIG_AKIRA_WASM_APP_PRIORITY,
        0, K_NO_WAIT);
    g_app_thread_created[instance_id] = true;

    if (!app->tid)
    {
        LOG_ERR("Failed to create app thread for slot %d", instance_id);
        app->status = AKIRA_APP_STATUS_CREATED;
        return -ENOMEM;
    }

    /* Block until WAMR instantiation succeeds or fails.
     * Hard timeout guards against WAMR deadlock or OOM. */
    int sem_ret = k_sem_take(&app->sem_start, K_MSEC(CONFIG_AKIRA_WASM_START_TIMEOUT_MS));
    if (sem_ret < 0)
    {
        LOG_ERR("Timeout waiting for WASM instantiation (slot %d) — aborting", instance_id);
        k_thread_abort(app->tid);
        k_thread_join(&g_app_threads[instance_id], K_MSEC(200));
        app->tid = NULL;
        /* Free the module and binary too, or each timed-out start leaks them. */
        k_mutex_lock(&app->exit_mutex, K_FOREVER);
        reclaim_dead_slot(instance_id, -ETIMEDOUT);
        app->status = AKIRA_APP_STATUS_ERROR;
        k_mutex_unlock(&app->exit_mutex);
        app->name[0] = '\0';
        app->used = false;
        return -ETIMEDOUT;
    }

    if (app->status == AKIRA_APP_STATUS_ERROR)
    {
        LOG_ERR("App failed to start (slot %d)", instance_id);
        k_thread_join(&g_app_threads[instance_id], K_MSEC(500));
        app->tid = NULL;
        return -EIO;
    }

    sandbox_audit_log(AUDIT_EVENT_APP_STARTED, app->name, (uint32_t)instance_id);
    LOG_INF("WASM module loaded into slot %d (cap=0x%016llx, quota=%u)",
            instance_id, (unsigned long long)app->cap_mask, app->memory_quota);
    return 0;
#else
    (void)instance_id;
    return -ENOTSUP;
#endif
}

/* Stop a running WASM app: terminate its thread and wait for cleanup */
int akira_runtime_stop(int instance_id)
{
    if (!slot_valid(instance_id))
    {
        return -EINVAL;
    }

    akira_managed_app_t *app = &g_apps[instance_id];

    if (app->parked ||
        app->status == AKIRA_APP_STATUS_STOPPED ||
        app->status == AKIRA_APP_STATUS_CREATED)
    {
        return 0; /* nothing to stop */
    }

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    /* The run this call stops. While it waits, the slot can be freed (the
     * app exits, or its crash is cleaned up) and reused by another app;
     * nothing below may touch that one. */
    const uint32_t gen = app->generation;

    if (app->tid && app->tid == k_current_get())
    {
        /* Called from the app's own thread (e.g. through a native): it can't
         * wait for itself to exit. Ask it to unwind; its thread finishes the
         * teardown when control returns to the runtime. */
        app->stop_requested = true;
        k_sem_give(&app->stop_sem);
        if (app->instance)
        {
            wasm_runtime_terminate(app->instance);
        }
        return 0;
    }

    /* Stage 1: ask. Set the flag and wake every wait the app can be in, then
     * terminate: WAMR sees the termination only when control returns to
     * WASM, so a native waiting on the app's behalf must be woken first. */
    if (app->instance)
    {
        app->stop_requested = true;
        akira_app_resources_on_stop_request(instance_id, app->name);
        k_sem_give(&app->stop_sem);
        wasm_runtime_terminate(app->instance);
    }

    /* Stage 2: grace. A cooperative app is gone within a frame.
     *
     * Stage 3: abort. A thread still on the CPU after the grace period is
     * spinning in WASM code (WAMR checks for termination only at calls), so
     * it holds no system lock and is aborted now. One blocked inside a native
     * call gets the full timeout. One inside a critical section
     * (native_critical) holds a system lock and is never aborted: it gets
     * three times the timeout to leave the section, and if it hasn't, the
     * stop gives up with -EBUSY and the app ends when the native returns.
     * A thread that crashed is cleaned up by the fault work, which this
     * waits for. */
    bool aborted = false;

    k_mutex_lock(&app->exit_mutex, K_FOREVER);
    if (app->status == AKIRA_APP_STATUS_RUNNING && app->generation == gen)
    {
        const int timeout = CONFIG_AKIRA_WASM_STOP_TIMEOUT_MS;
        const int grace = MIN(1500, timeout);
        int waited = grace;
        int wait_ret = k_condvar_wait(&app->cond_exit, &app->exit_mutex, K_MSEC(grace));

        while (wait_ret != 0 && app->status == AKIRA_APP_STATUS_RUNNING && app->tid &&
               app->used && app->generation == gen)
        {
            uint32_t state = app->tid->base.thread_state;
            bool blocked = (state & (_THREAD_PENDING | _THREAD_SLEEPING | _THREAD_SUSPENDED)) != 0;
            bool critical = atomic_get(&app->native_critical) > 0;

            if (!((app->crashed && waited < 3 * timeout) ||
                  (critical && waited < 3 * timeout) ||
                  (blocked && waited < timeout)))
            {
                break;
            }
            wait_ret = k_condvar_wait(&app->cond_exit, &app->exit_mutex, K_MSEC(100));
            waited += 100;
        }
        if (wait_ret != 0 && app->status == AKIRA_APP_STATUS_RUNNING && !app->crashed &&
            app->used && app->generation == gen && app->tid)
        {
            bool blocked = (app->tid->base.thread_state &
                            (_THREAD_PENDING | _THREAD_SLEEPING | _THREAD_SUSPENDED)) != 0;

            /* Suspended, it can't enter a critical section between the check
             * and the abort. */
            k_thread_suspend(app->tid);
            if (atomic_get(&app->native_critical) > 0)
            {
                k_thread_resume(app->tid);
                k_mutex_unlock(&app->exit_mutex);
                LOG_ERR("App thread (slot %d) is still inside a lock-holding native after "
                        "%d ms: not aborting it (the lock would be lost); it stops when "
                        "the native returns", instance_id, 3 * timeout);
                return -EBUSY;
            }
            LOG_ERR("App thread (slot %d) didn't stop: aborting it (%s)", instance_id,
                    blocked ? "still blocked in a native call after the full timeout"
                            : "spinning in WASM code");
            k_thread_abort(app->tid);
            app->tid = NULL;
            /* Stopped on request: report it as a clean stop (exit code 0). */
            reclaim_dead_slot(instance_id, 0);
            aborted = true;
        }
    }
    if (!app->used || app->generation != gen)
    {
        /* The app ended and its slot was freed (and maybe reused) while this
         * waited: nothing of this run is left to stop. */
        k_mutex_unlock(&app->exit_mutex);
        return 0;
    }
    /* A crashed app whose cleanup hasn't run yet stays RUNNING, so a new
     * start can't take the slot before the fault work frees it. */
    if (!(app->crashed && app->status == AKIRA_APP_STATUS_RUNNING))
    {
        app->status = AKIRA_APP_STATUS_STOPPED;
    }
    k_tid_t tid = app->tid;
    k_mutex_unlock(&app->exit_mutex);

    /* Join to release Zephyr thread resources. Timeout is 2 s: the join is
     * always called without the outer g_registry_mutex held so the WASM
     * thread's exit callback can complete without deadlock. */
    if (tid)
    {
        k_thread_join(tid, K_MSEC(2000));
        k_mutex_lock(&app->exit_mutex, K_FOREVER);
        if (app->generation == gen && app->tid == tid)
        {
            app->tid = NULL;
        }
        k_mutex_unlock(&app->exit_mutex);
    }

    sandbox_audit_log(AUDIT_EVENT_APP_STOPPED, app->name, (uint32_t)instance_id);
    LOG_INF("Stopped slot %d (calls=%u, traps=%u)",
            instance_id, app->perf.call_count, app->perf.trap_count);
    if (aborted)
    {
        /* The thread never reached its own exit path: free the slot here,
         * or every aborted app would leak a runtime slot. */
        free_dead_slot(instance_id, 0);
    }
    return 0;
#else
    (void)instance_id;
    return -ENOTSUP;
#endif
}

int akira_runtime_install_with_manifest(const char *name, const void *binary, size_t size, const char *manifest_json, size_t manifest_size)
{
    return akira_runtime_install_admitted(name, binary, size, manifest_json,
                                          manifest_size, NULL);
}

int akira_runtime_install_admitted(const char *name, const void *binary, size_t size,
                                   const char *manifest_json, size_t manifest_size,
                                   const struct akira_admission_record *record)
{
    if (!name || !binary || size == 0)
        return -EINVAL;

    /* Save manifest if provided (for external tools/debugging) */
    if (manifest_json && manifest_size > 0)
    {
        char mpath[FILE_DIR_MAX_LEN];
        snprintf(mpath, sizeof(mpath), "/lfs/apps/%s.manifest.json", name);
        if (fs_manager_exists(mpath))
        {
            ssize_t mv = fs_manager_write_file(mpath, manifest_json, manifest_size);
            if (mv != (ssize_t)manifest_size)
            {
                LOG_WRN("Failed to write manifest fully for %s", name);
            }
            else
            {
                LOG_INF("Saved manifest to %s", mpath);
            }
        }
        else
        {
            LOG_WRN("Filesystem not available for manifest save");
        }
    }

    /* Load into runtime memory. The embedded manifest wins; manifest_json is
     * used only when the binary has none, and capabilities, quota and commands
     * all come from that one manifest (see load_wasm_ex). */
    int id = load_wasm_ex((const uint8_t *)binary, (uint32_t)size,
                          manifest_json, manifest_size, name, record);
    if (id < 0)
        return id;

    /* Store friendly name */
    strncpy(g_apps[id].name, name, sizeof(g_apps[id].name) - 1);
    g_apps[id].name[sizeof(g_apps[id].name) - 1] = '\0';

    return id;
}

int akira_runtime_install(const char *name, const void *binary, size_t size)
{
    return akira_runtime_install_with_manifest(name, binary, size, NULL, 0);
}

/* Destroy: stop if running, then fully unload the module and free the slot */
int akira_runtime_destroy(int instance_id)
{
    if (!slot_valid(instance_id))
    {
        return -EINVAL;
    }

    akira_managed_app_t *app = &g_apps[instance_id];

    if (app->parked)
    {
        /* Its resources may still be in use (see fault_work_fn()): the slot
         * stays reserved until reboot. */
        return 0;
    }

    /* Stop the thread first if it's still running */
    if (app->status == AKIRA_APP_STATUS_RUNNING ||
        app->status == AKIRA_APP_STATUS_EXITED)
    {
        akira_runtime_stop(instance_id);
    }

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    /* Safety: instance should already be NULL after stop,
     * but guard against the k_thread_abort path in stop().
     * exec_env is the singleton — freed automatically by deinstantiate, never
     * call wasm_runtime_destroy_exec_env on it. */
    app->exec_env = NULL;
    if (app->instance)
    {
        AKIRA_DISPLAY_RELEASE_OWNER(app->instance);
        instance_map_remove(app->instance);
        wasm_runtime_deinstantiate(app->instance);
        app->instance = NULL;
    }

    /* Release module cache reference */
    if (app->hash_valid)
    {
        module_cache_release(app->binary_hash);
    }

    if (app->module)
    {
        wasm_runtime_unload(app->module);
        app->module = NULL;
    }
    if (app->hash_valid)
    {
        module_cache_invalidate(app->binary_hash);
    }
    /* Free owned binary AFTER unload — WAMR keeps raw string pointers into it */
    if (app->wasm_binary)
    {
        akira_free_buffer(app->wasm_binary);
        app->wasm_binary = NULL;
    }
#endif

    app->used = false;
    app->status = AKIRA_APP_STATUS_STOPPED;
    app->exit_code = 0;
    app->tid = NULL;
    app->cap_mask = 0;
    app->hash_valid = false;
    app->name[0] = '\0';
    memset(&app->sandbox, 0, sizeof(sandbox_ctx_t));
    memset(&app->perf, 0, sizeof(runtime_perf_stats_t));
    return 0;
}

/* Uninstall: remove persistent files and destroy runtime slot */
int akira_runtime_uninstall(const char *name, int instance_id)
{
    if (!name)
        return -EINVAL;

    /* Stop/destroy if running */
    if (instance_id >= 0)
    {
        akira_runtime_stop(instance_id);
        akira_runtime_destroy(instance_id);
    }

    return 0;
}

/* End of file */

/* ===== New API Functions (v2) ===== */

sandbox_ctx_t *akira_runtime_get_sandbox(int instance_id)
{
    if (!slot_valid(instance_id))
        return NULL;
    return &g_apps[instance_id].sandbox;
}

runtime_perf_stats_t *akira_runtime_get_perf_stats(int instance_id)
{
    if (!slot_valid(instance_id))
        return NULL;
    return &g_apps[instance_id].perf;
}

int akira_runtime_verify_binary(const uint8_t *binary, uint32_t size,
                                uint8_t *hash_out)
{
    return app_verify_wasm_integrity(binary, size, hash_out);
}

void akira_runtime_set_exit_callback(akira_runtime_exit_cb_t cb)
{
    /* Protect the pointer write so concurrent reads on the exit path see
     * either the old or the new value atomically (no torn pointer). */
    k_mutex_lock(&g_runtime_mutex, K_FOREVER);
    g_exit_cb = cb;
    k_mutex_unlock(&g_runtime_mutex);
}

void akira_runtime_get_cache_stats(module_cache_stats_t *stats)
{
    module_cache_get_stats(stats);
}
