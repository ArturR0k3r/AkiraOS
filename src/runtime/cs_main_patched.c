/**
 * @file cs_main_patched.c
 * @brief Patched version of OCRE Container Supervisor main
 * 
 * This is a patched version of the OCRE cs_main.c that allows
 * configurable thread stack size via CONFIG_AKIRA_OCRE_CS_THREAD_STACK_SIZE.
 * 
 * Original copyright:
 * @copyright Copyright © contributors to Project Ocre,
 * which has been established as Project Ocre a Series of LF Projects, LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ocre/ocre.h>
#include "cs_sm.h"
#include "ocre_core_external.h"
#include <zephyr/kernel.h>
#include <zephyr/init.h>

/* ESP32/Xtensa: Include core-isa.h for XCHAL_HAVE_THREADPTR */
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
#include <xtensa/config/core-isa.h>
#endif

LOG_MODULE_REGISTER(ocre_cs_component, OCRE_LOG_LEVEL);

/* Debug: Track when this file is loaded */
static int cs_main_patched_marker(void) {
    printk("*** CS_MAIN_PATCHED MODULE LOADED (SYS_INIT) ***\n");
    return 0;
}
SYS_INIT(cs_main_patched_marker, APPLICATION, 99);

/* 
 * AkiraOS Patch: Static stack for Container Supervisor thread
 * 
 * We use a hardcoded stack size for K_THREAD_STACK_DEFINE because:
 * 1. The macro is evaluated at compile time, before Kconfig values are available
 * 2. Dynamic allocation via k_thread_stack_alloc doesn't work properly on ESP32-S3
 *    (causes TLS initialization issues with THREADPTR=0xaaaaaaaa)
 * 
 * Stack sizes:
 * - ESP32/ESP32-S3: 16384 bytes (larger due to WAMR requirements)
 * - Other platforms: 8192 bytes
 */
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
#define OCRE_CS_STATIC_STACK_SIZE 16384
#else
#define OCRE_CS_STATIC_STACK_SIZE 8192
#endif

/* Thread priority: use preemptive priority on ESP32 */
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
#define OCRE_CS_THREAD_PRIORITY   5
#else
#define OCRE_CS_THREAD_PRIORITY   0
#endif

/* 
 * AkiraOS Patch: Use static stack allocation instead of dynamic
 * This ensures proper memory alignment and TLS setup on all platforms.
 */
K_THREAD_STACK_DEFINE(ocre_cs_stack, OCRE_CS_STATIC_STACK_SIZE);
static struct k_thread ocre_cs_thread_data;
static k_tid_t ocre_cs_tid = NULL;
static int ocre_cs_thread_initialized = 0;

static void ocre_cs_main(void *p1, void *p2, void *p3) {
    ocre_cs_ctx *ctx = (ocre_cs_ctx *)p1;
    
    /*
     * AkiraOS Patch: Initialize THREADPTR to 0 on ESP32/Xtensa
     * 
     * Zephyr doesn't initialize THREADPTR unless CONFIG_THREAD_LOCAL_STORAGE
     * or CONFIG_USERSPACE is enabled. On ESP32-S3, an uninitialized THREADPTR
     * (containing garbage like 0xaaaaaaaa from stack sentinel) causes crashes
     * when WAMR or other code tries to access TLS data.
     * 
     * Setting THREADPTR to 0 prevents this crash. Code that checks for valid
     * TLS will see NULL and handle it appropriately.
     */
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
#if XCHAL_HAVE_THREADPTR
    {
        uintptr_t zero = 0;
        __asm__ volatile("wur.THREADPTR %0" : : "r"(zero));
    }
    LOG_INF("THREADPTR initialized to 0 for ESP32");
#endif
#endif
    
    LOG_INF("Container Supervisor thread starting (stack=%d)...", OCRE_CS_STATIC_STACK_SIZE);
    
    wasm_runtime_init_thread_env();
    LOG_INF("WAMR thread environment initialized");
    
    LOG_INF("Starting Container Supervisor state machine...");
    int ret = _ocre_cs_run(ctx);
    LOG_ERR("Container Supervisor exited: %d", ret);
    
    wasm_runtime_destroy_thread_env();
}

// Function to start the thread
void start_ocre_cs_thread(ocre_cs_ctx *ctx) {
    if (ocre_cs_thread_initialized) {
        LOG_WRN("Container Supervisor thread is already running.");
        return;
    }
    
    LOG_INF("Creating Container Supervisor thread (stack=%d, prio=%d)", 
           OCRE_CS_STATIC_STACK_SIZE, OCRE_CS_THREAD_PRIORITY);
    
    /*
     * AkiraOS Patch: Create thread with K_FOREVER delay, then patch TLS and start
     * 
     * On ESP32-S3/Xtensa, Zephyr doesn't initialize THREADPTR unless
     * CONFIG_THREAD_LOCAL_STORAGE or CONFIG_USERSPACE is enabled.
     * By creating the thread suspended (K_FOREVER), we can patch the
     * thread's tls field to 0 before starting it.
     */
    ocre_cs_tid = k_thread_create(&ocre_cs_thread_data, 
                                   ocre_cs_stack,
                                   K_THREAD_STACK_SIZEOF(ocre_cs_stack),
                                   ocre_cs_main,
                                   ctx, NULL, NULL,
                                   OCRE_CS_THREAD_PRIORITY, 0, K_FOREVER);
    
    if (ocre_cs_tid == NULL) {
        LOG_ERR("Failed to create Container Supervisor thread");
        return;
    }
    
    k_thread_name_set(ocre_cs_tid, "Ocre Container Supervisor");
    
    /*
     * AkiraOS Patch: Fix THREADPTR initialization for ESP32/Xtensa
     * 
     * Zephyr's init_stack() only initializes frame->bsa.threadptr when
     * CONFIG_THREAD_LOCAL_STORAGE or CONFIG_USERSPACE is enabled.
     * Without these configs, threadptr contains garbage (0xaaaaaaaa).
     * 
     * The thread's switch_handle points to ptr_to_bsa which points to BSA.
     * The threadptr field is at offset 0 in the BSA structure.
     * 
     * Structure: switch_handle -> ptr_to_bsa -> bsa.threadptr (offset 0)
     */
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
    if (ocre_cs_thread_data.switch_handle != NULL) {
        /* switch_handle points to ptr_to_bsa */
        void **ptr_to_bsa = (void **)ocre_cs_thread_data.switch_handle;
        if (*ptr_to_bsa != NULL) {
            /* BSA.threadptr is at offset 0, so *ptr_to_bsa points directly to it */
            uintptr_t *threadptr_ptr = (uintptr_t *)*ptr_to_bsa;
            *threadptr_ptr = 0;  /* Clear THREADPTR in BSA */
            LOG_DBG("Patched BSA threadptr to 0 at %p", (void *)threadptr_ptr);
        }
    }
#endif
    
    /* Now start the thread */
    k_thread_start(ocre_cs_tid);
    
    ocre_cs_thread_initialized = 1;
    LOG_INF("Container Supervisor thread created and started successfully");
}

void destroy_ocre_cs_thread(void) {
    if (!ocre_cs_thread_initialized) {
        LOG_ERR("Container Supervisor thread is not running.\n");
        return;
    }
    k_thread_abort(ocre_cs_tid);
    ocre_cs_thread_initialized = 0;
}
