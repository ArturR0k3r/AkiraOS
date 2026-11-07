cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(exec_speed)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${ZEPHYR_WASM_MICRO_RUNTIME_MODULE_DIR}/core/iwasm/include
)

# ── WAMR (classic interpreter; AOT toggled by -DEXEC_SPEED_AOT=y) ─────────────
set(WAMR_ROOT_DIR ${ZEPHYR_WASM_MICRO_RUNTIME_MODULE_DIR})
set(WAMR_BUILD_PLATFORM "zephyr")
string(TOLOWER "${CMAKE_C_COMPILER}" _cc)
if(_cc MATCHES "xtensa")
    set(WAMR_BUILD_TARGET "XTENSA_32")
elseif(_cc MATCHES "riscv")
    set(WAMR_BUILD_TARGET "RISCV32")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WAMR_BUILD_TARGET "X86_64")
else()
    set(WAMR_BUILD_TARGET "X86_32")
endif()
set(WAMR_BUILD_INTERP        1)
set(WAMR_BUILD_MINI_LOADER   0)   # full loader so it can also load .aot
# AOT: run the embedded ESP32-S3 .aot (exec_aot.h) alongside interp+native.
# Auto-enabled on ESP32-S3 + PSRAM (the board conf that makes AOT exec memory
# available); can also be forced with -DEXEC_SPEED_AOT=y. Never on native_sim
# (exec_aot.h is Xtensa machine code).
if(NOT DEFINED EXEC_SPEED_AOT AND CONFIG_SOC_SERIES_ESP32S3 AND CONFIG_ESP_SPIRAM)
    set(EXEC_SPEED_AOT y)
endif()
if(EXEC_SPEED_AOT)
    set(WAMR_BUILD_AOT 1)
    target_compile_definitions(app PRIVATE EXEC_SPEED_AOT=1)
    message(STATUS "exec_speed: AOT path ENABLED")
    # ESP32-S3: .aot native code lives in PSRAM and executes through the
    # instruction-bus alias; the loader must write/relocate via the data-bus
    # mirror (see zephyr_platform.c aot_exec_alloc + os_get_dbus_mirror).
    # Mirror the production zephyr/CMakeLists.txt AOT block exactly.
    if(CONFIG_SOC_SERIES_ESP32S3 AND CONFIG_ESP_SPIRAM)
        add_definitions(-DWASM_MEM_DUAL_BUS_MIRROR=1)
        message(STATUS "exec_speed: WAMR dual-bus mirror ENABLED (PSRAM instruction cache)")
    endif()
else()
    set(WAMR_BUILD_AOT 0)
endif()
set(WAMR_BUILD_LIBC_BUILTIN  1)
set(WAMR_BUILD_LIBC_WASI     0)
set(WAMR_BUILD_GLOBAL_HEAP_POOL 0)
set(WAMR_BUILD_REF_TYPES     1)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
# ESP32-S3 AOT dual-bus fix: swap in a patched zephyr_platform.c whose
# os_dcache_flush() uses the ESP ROM cache primitives (Cache_WriteBack_All +
# Cache_Invalidate_ICache_All). The stock version calls sys_cache_*, which are
# no-ops on this SoC (CONFIG_CACHE has no esp32s3 driver), so the AOT loader read
# stale cache after the DBUS code write (literal_size came back as garbage ->
# corrupt module->code -> "target address out of range"). This fix belongs
# upstream in the WAMR module; it is applied here so the bench can run AOT.
if(EXEC_SPEED_AOT)
    list(FILTER WAMR_RUNTIME_LIB_SOURCE EXCLUDE REGEX "zephyr_platform\\.c$")
    list(APPEND WAMR_RUNTIME_LIB_SOURCE ${CMAKE_CURRENT_LIST_DIR}/wamr_patch/zephyr_platform.c)
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/wamr_patch/zephyr_platform.c PROPERTIES
        INCLUDE_DIRECTORIES "${WAMR_ROOT_DIR}/core/shared/platform/include;${WAMR_ROOT_DIR}/core/shared/platform/zephyr;${WAMR_ROOT_DIR}/core/shared/utils;${WAMR_ROOT_DIR}/core/iwasm/include")
endif()
target_sources(app PRIVATE ${WAMR_RUNTIME_LIB_SOURCE})
foreach(_wamr_src ${WAMR_RUNTIME_LIB_SOURCE})
    if(_wamr_src MATCHES "\\.(c|cc|cpp|cxx)$")
        set_source_files_properties(${_wamr_src} PROPERTIES COMPILE_OPTIONS "-include;zephyr/fs/fs.h")
    endif()
endforeach()
target_include_directories(app PRIVATE
    ${WAMR_ROOT_DIR}/core/iwasm/include
    ${WAMR_ROOT_DIR}/core/shared
    ${WAMR_ROOT_DIR}/core/deps
)
