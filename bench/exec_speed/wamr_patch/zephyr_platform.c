/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-FileCopyrightText: 2024 Siemens AG (For Zephyr usermode changes)
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"

/* function pointers for executable memory management */
static exec_mem_alloc_func_t exec_mem_alloc_func = NULL;
static exec_mem_free_func_t exec_mem_free_func = NULL;

#if WASM_ENABLE_AOT != 0
#ifdef CONFIG_SOC_SERIES_ESP32S3
/*
 * ESP32-S3 PSRAM instruction-cache approach:
 *
 * AOT code is allocated in PSRAM (data-bus 0x3C000000–0x3DFFFFFF) and
 * executed through the instruction-bus alias (0x42000000–0x43FFFFFF).
 * The ESP32-S3 MMU table is shared: a single entry covers both buses,
 * so an existing DBUS mapping at 0x3Cxxxxxx is also valid for IBUS
 * fetch at 0x42xxxxxx.  The CPU's L1 I-cache (16 KB default) auto-
 * matically caches hot code blocks from PSRAM — no software overlay
 * manager is required.
 *
 * WASM_MEM_DUAL_BUS_MIRROR makes the AOT loader write via the DBUS
 * mirror (os_get_dbus_mirror) and flush caches (os_dcache_flush)
 * before execution.
 *
 */
#include <zephyr/multi_heap/shared_multi_heap.h>

/* External memory address ranges */
#define ESP32S3_PSRAM_DBUS_LOW   0x3C000000
#define ESP32S3_PSRAM_DBUS_HIGH  0x3E000000
#define ESP32S3_PSRAM_IBUS_LOW   0x42000000
#define ESP32S3_PSRAM_IBUS_HIGH  0x44000000
#define ESP32S3_DBUS_IBUS_OFFSET 0x6000000

#define ESP32S3_DBUS_TO_IBUS(addr) \
    ((void *)((uintptr_t)(addr) + ESP32S3_DBUS_IBUS_OFFSET))
#define ESP32S3_IBUS_TO_DBUS(addr) \
    ((void *)((uintptr_t)(addr) - ESP32S3_DBUS_IBUS_OFFSET))

static void *
aot_exec_alloc(size_t size)
{
    void *dbus = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL,
                                                 16, size);
    if (!dbus) {
        os_printf("aot_exec_alloc: PSRAM alloc(%zu) failed\n", size);
        return NULL;
    }
    uintptr_t d = (uintptr_t)dbus;
    if (d < ESP32S3_PSRAM_DBUS_LOW || d >= ESP32S3_PSRAM_DBUS_HIGH) {
        os_printf("aot_exec_alloc: 0x%lx not in PSRAM DBUS range\n",
                  (unsigned long)d);
        shared_multi_heap_free(dbus);
        return NULL;
    }
    void *ibus = ESP32S3_DBUS_TO_IBUS(dbus);
    os_printf("aot_exec_alloc: %zu bytes PSRAM DBUS=%p IBUS=%p\n",
              size, dbus, ibus);
    return ibus;
}

static void
aot_exec_free(void *ibus_ptr)
{
    if (!ibus_ptr)
        return;
    void *dbus = ESP32S3_IBUS_TO_DBUS(ibus_ptr);
    os_printf("aot_exec_free: IBUS=%p DBUS=%p\n", ibus_ptr, dbus);
    shared_multi_heap_free(dbus);
}

void *
os_get_dbus_mirror(void *ibus)
{
    uintptr_t a = (uintptr_t)ibus;
    if (a >= ESP32S3_PSRAM_IBUS_LOW && a < ESP32S3_PSRAM_IBUS_HIGH)
        return ESP32S3_IBUS_TO_DBUS(ibus);
    return ibus;
}
#endif /* CONFIG_SOC_SERIES_ESP32S3 */
#endif /* WASM_ENABLE_AOT */

#if WASM_ENABLE_AOT != 0
#ifdef CONFIG_ARM_MPU
/**
 * This function will allow execute from sram region.
 * This is needed for AOT code because by default all soc will
 * disable the execute from SRAM.
 */
static void
disable_mpu_rasr_xn(void)
{
    uint32 index;
    /* Kept the max index as 8 (irrespective of soc) because the sram
       would most likely be set at index 2. */
    for (index = 0U; index < 8; index++) {
        MPU->RNR = index;
#ifdef MPU_RASR_XN_Msk
        if (MPU->RASR & MPU_RASR_XN_Msk) {
            MPU->RASR |= ~MPU_RASR_XN_Msk;
        }
#endif
    }
}
#endif /* end of CONFIG_ARM_MPU */
#endif

#ifndef CONFIG_USERSPACE
static int
_stdout_hook_iwasm(int c)
{
    printk("%c", (char)c);
    return 1;
}
#endif

int
os_thread_sys_init();

void
os_thread_sys_destroy();

int
bh_platform_init()
{
#ifndef CONFIG_USERSPACE
    extern void __stdout_hook_install(int (*hook)(int));
    /* Enable printf() in Zephyr */
    __stdout_hook_install(_stdout_hook_iwasm);
#endif

#if WASM_ENABLE_AOT != 0
#ifdef CONFIG_ARM_MPU
    /* Enable executable memory support */
    disable_mpu_rasr_xn();
#endif
#endif

    return os_thread_sys_init();
}

void
bh_platform_destroy()
{
    os_thread_sys_destroy();
}

void *
os_malloc(unsigned size)
{
    return malloc(size);
}

void *
os_realloc(void *ptr, unsigned size)
{
    return realloc(ptr, size);
}

void
os_free(void *ptr)
{
    free(ptr);
}

int
os_dumps_proc_mem_info(char *out, unsigned int size)
{
    return -1;
}

#if 0
struct out_context {
    int count;
};

typedef int (*out_func_t)(int c, void *ctx);

static int
char_out(int c, void *ctx)
{
    struct out_context *out_ctx = (struct out_context*)ctx;
    out_ctx->count++;
    return _stdout_hook_iwasm(c);
}

int
os_vprintf(const char *fmt, va_list ap)
{
#if 0
    struct out_context ctx = { 0 };
    cbvprintf(char_out, &ctx, fmt, ap);
    return ctx.count;
#else
    vprintk(fmt, ap);
    return 0;
#endif
}
#endif

int
os_printf(const char *format, ...)
{
    int ret = 0;
    va_list ap;

    va_start(ap, format);
#ifndef BH_VPRINTF
    ret += vprintf(format, ap);
#else
    ret += BH_VPRINTF(format, ap);
#endif
    va_end(ap);

    return ret;
}

int
os_vprintf(const char *format, va_list ap)
{
#ifndef BH_VPRINTF
    return vprintf(format, ap);
#else
    return BH_VPRINTF(format, ap);
#endif
}

#if KERNEL_VERSION_NUMBER <= 0x020400 /* version 2.4.0 */
void
abort(void)
{
    int i = 0;
    os_printf("%d\n", 1 / i);
}
#endif

#if KERNEL_VERSION_NUMBER <= 0x010E01 /* version 1.14.1 */
size_t
strspn(const char *s, const char *accept)
{
    os_printf("## unimplemented function %s called", __FUNCTION__);
    return 0;
}

size_t
strcspn(const char *s, const char *reject)
{
    os_printf("## unimplemented function %s called", __FUNCTION__);
    return 0;
}
#endif

void *
os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    void *addr;

    if ((uint64)size >= UINT32_MAX)
        return NULL;

#if WASM_ENABLE_AOT != 0 && defined(CONFIG_SOC_SERIES_ESP32S3)
    if (prot & MMAP_PROT_EXEC) {
        addr = aot_exec_alloc(size);
        if (addr) {
            void *dbus = ESP32S3_IBUS_TO_DBUS(addr);
            memset(dbus, 0, size);
        }
        return addr;
    }
#endif

    if (exec_mem_alloc_func)
        addr = exec_mem_alloc_func((uint32)size);
    else
        addr = BH_MALLOC(size);

    if (addr)
        memset(addr, 0, size);
    return addr;
}

void *
os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    return os_mremap_slow(old_addr, old_size, new_size);
}

void
os_munmap(void *addr, size_t size)
{
#if WASM_ENABLE_AOT != 0 && defined(CONFIG_SOC_SERIES_ESP32S3)
    uintptr_t a = (uintptr_t)addr;
    if (a >= ESP32S3_PSRAM_IBUS_LOW && a < ESP32S3_PSRAM_IBUS_HIGH) {
        aot_exec_free(addr);
        return;
    }
#endif
    if (exec_mem_free_func)
        exec_mem_free_func(addr);
    else
        BH_FREE(addr);
}

int
os_mprotect(void *addr, size_t size, int prot)
{
    return 0;
}

void
os_dcache_flush()
{
#if defined(CONFIG_CPU_CORTEX_M7) && defined(CONFIG_ARM_MPU)
#if KERNEL_VERSION_NUMBER < 0x030300 /* version 3.3.0 */
    uint32 key;
    key = irq_lock();
    SCB_CleanDCache();
    irq_unlock(key);
#else
    sys_cache_data_flush_all();
#endif
#elif defined(CONFIG_SOC_CVF_EM7D) && defined(CONFIG_ARC_MPU) \
    && defined(CONFIG_CACHE_FLUSHING)
    __asm__ __volatile__("sync");
    z_arc_v2_aux_reg_write(_ARC_V2_DC_FLSH, BIT(0));
    __asm__ __volatile__("sync");
#elif defined(CONFIG_SOC_SERIES_ESP32S3)
    /* Flush data-cache write-backs to PSRAM and invalidate I-cache so the CPU
     * AND the AOT loader's instruction-bus reads of section metadata see the
     * freshly DBUS-written code. The Zephyr sys_cache_* ops are no-ops on this
     * SoC (CONFIG_CACHE has no esp32s3 driver), which left dual-bus AOT loads
     * reading stale cache (literal_size came back as garbage -> corrupt
     * module->code -> "target address out of range"). Use the ESP ROM cache
     * primitives, which are always linked. */
    {
        extern void Cache_WriteBack_All(void);
        extern void Cache_Invalidate_ICache_All(void);
        Cache_WriteBack_All();
        Cache_Invalidate_ICache_All();
    }
#endif
}

void
os_icache_flush(void *start, size_t len)
{
#if KERNEL_VERSION_NUMBER >= 0x030300 /* version 3.3.0 */
    sys_cache_instr_flush_range(start, len);
#endif
}

void
set_exec_mem_alloc_func(exec_mem_alloc_func_t alloc_func,
                        exec_mem_free_func_t free_func)
{
    exec_mem_alloc_func = alloc_func;
    exec_mem_free_func = free_func;
}

os_raw_file_handle
os_invalid_raw_handle(void)
{
    return -1;
}
