/**
 * @file ocre_config_override.h
 * @brief AkiraOS overrides for OCRE configuration
 * 
 * This header provides post-include fixups for OCRE configuration values.
 * It's designed to be included AFTER OCRE headers, undoing their hardcoded
 * definitions and replacing them with configurable values.
 * 
 * Usage: Include this header after including any OCRE headers that define
 * constants you want to override.
 */

#ifndef OCRE_CONFIG_OVERRIDE_H
#define OCRE_CONFIG_OVERRIDE_H

/* 
 * Override OCRE Container Supervisor thread stack size
 * The OCRE header core_internal.h hardcodes this to 4096, which is too small
 * for ESP32-S3. We undef and redefine with the configured value.
 */
#ifdef CONFIG_AKIRA_OCRE_CS_THREAD_STACK_SIZE
    #ifdef OCRE_CS_THREAD_STACK_SIZE
        #undef OCRE_CS_THREAD_STACK_SIZE
    #endif
    #define OCRE_CS_THREAD_STACK_SIZE CONFIG_AKIRA_OCRE_CS_THREAD_STACK_SIZE
#endif

#endif /* OCRE_CONFIG_OVERRIDE_H */
