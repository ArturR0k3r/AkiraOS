# SPDX-License-Identifier: Apache-2.0
#
# WAMR Configuration Override for Embedded Systems
# 
# This file patches WAMR configuration to use builtin libc for embedded
# targets (XTENSA, RISC-V) since WASI libc requires POSIX headers
# (sys/random.h) that are not available on these platforms.
#
# This override is applied BEFORE wamr.cmake to ensure our settings
# take precedence.

message(STATUS "[AkiraOS WAMR Override] Applying embedded system configuration")

# Add compiler flags to ensure Zephyr filesystem headers are available
# This fixes incomplete type errors in platform_internal.h
add_compile_options(-include zephyr/fs/fs.h)

# Detect target architecture from Kconfig
if(CONFIG_XTENSA OR CONFIG_RISCV OR CONFIG_ARM OR CONFIG_ESP32)
    message(STATUS "[AkiraOS WAMR Override] Embedded target detected (XTENSA/RISC-V/ARM)")
    
    # Use WAMR's builtin libc for embedded systems
    set(WAMR_BUILD_LIBC_BUILTIN 1 CACHE BOOL "Use WAMR builtin libc" FORCE)
    set(WAMR_BUILD_LIBC_WASI 0 CACHE BOOL "Disable WASI libc" FORCE)
    set(WAMR_BUILD_LIB_WASI_THREADS 0 CACHE BOOL "Disable WASI threads" FORCE)
    set(WAMR_BUILD_LIB_PTHREAD 0 CACHE BOOL "Disable pthread" FORCE)
    
    message(STATUS "[AkiraOS WAMR Override] Using WAMR builtin libc")
    
else()
    message(STATUS "[AkiraOS WAMR Override] Host/POSIX target detected")
    
    # For POSIX systems, WASI libc is acceptable
    set(WAMR_BUILD_LIBC_BUILTIN 0 CACHE BOOL "Disable builtin libc" FORCE)
    set(WAMR_BUILD_LIBC_WASI 1 CACHE BOOL "Use WASI libc" FORCE)
    set(WAMR_BUILD_LIB_WASI_THREADS 1 CACHE BOOL "Enable WASI threads" FORCE)
    set(WAMR_BUILD_LIB_PTHREAD 1 CACHE BOOL "Enable pthread" FORCE)
    
    message(STATUS "[AkiraOS WAMR Override] Using WASI libc")
endif()
