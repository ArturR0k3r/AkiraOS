cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cg_microbench)

# Capability registry iterable section (_akira_capability_list_start/end).
zephyr_linker_sources(SECTIONS ${CMAKE_CURRENT_LIST_DIR}/../../zephyr/linker/akira_sections.ld)

set(AKIRA_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)

target_sources(app PRIVATE
    src/main.c
    # The real Capability Guard under test.
    ${AKIRA_SRC}/runtime/security.c
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${AKIRA_SRC}
    ${AKIRA_SRC}/runtime
    ${AKIRA_SRC}/runtime/security
    ${AKIRA_SRC}/api
    ${CMAKE_CURRENT_LIST_DIR}/../../include
    ${ZEPHYR_WASM_MICRO_RUNTIME_MODULE_DIR}/core/iwasm/include
)

# security.c is part of the AkiraOS module normally; built standalone here, so
# turn on the switches its real (non-stub) code path needs.
target_compile_definitions(app PRIVATE
    CONFIG_AKIRA_WASM_RUNTIME=1
    CONFIG_AKIRA_CAPABILITY_SYSTEM=1
    CONFIG_AKIRA_LOG_LEVEL=3
    CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK=0xFFFFFFFFFFFFFFFFULL
)

# ── WAMR (classic interpreter, mini-loader), mirroring akiraclaw_bench ───────
set(WAMR_ROOT_DIR ${ZEPHYR_WASM_MICRO_RUNTIME_MODULE_DIR})
set(WAMR_BUILD_PLATFORM "zephyr")
string(TOLOWER "${CMAKE_C_COMPILER}" _cc)
if(_cc MATCHES "xtensa")
    set(WAMR_BUILD_TARGET "XTENSA_32")
elseif(_cc MATCHES "riscv")
    set(WAMR_BUILD_TARGET "RISCV32")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WAMR_BUILD_TARGET "X86_64")   # native_sim on a 64-bit host
else()
    set(WAMR_BUILD_TARGET "X86_32")
endif()
set(WAMR_BUILD_INTERP        1)
set(WAMR_BUILD_MINI_LOADER   1)
set(WAMR_BUILD_AOT           0)
set(WAMR_BUILD_LIBC_BUILTIN  1)
set(WAMR_BUILD_LIBC_WASI     0)
set(WAMR_BUILD_GLOBAL_HEAP_POOL 0)
set(WAMR_BUILD_REF_TYPES     1)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
target_sources(app PRIVATE ${WAMR_RUNTIME_LIB_SOURCE})

# WAMR's zephyr platform header uses struct fs_file_t/fs_dir_t without including
# <zephyr/fs/fs.h>; force-include it for the WAMR C/C++ sources (not the .S asm).
foreach(_wamr_src ${WAMR_RUNTIME_LIB_SOURCE})
    if(_wamr_src MATCHES "\\.(c|cc|cpp|cxx)$")
        set_source_files_properties(${_wamr_src} PROPERTIES
            COMPILE_OPTIONS "-include;zephyr/fs/fs.h")
    endif()
endforeach()

target_include_directories(app PRIVATE
    ${WAMR_ROOT_DIR}/core/iwasm/include
    ${WAMR_ROOT_DIR}/core/shared
    ${WAMR_ROOT_DIR}/core/deps
)
