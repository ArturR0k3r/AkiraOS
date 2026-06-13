cmake_minimum_required(VERSION 3.20)

# ── AkiraOS board/DTS/Kconfig roots ─────────────────────────────────────
# Must be set BEFORE find_package(Zephyr) so the AKIRA_* Kconfig symbols
# and the akiraconsole board definition are available during configuration.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
list(APPEND DTS_ROOT   ${CMAKE_CURRENT_LIST_DIR}/../..)
set(KCONFIG_ROOT       ${CMAKE_CURRENT_LIST_DIR}/../../Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(akiraclaw_bench VERSION 1.0.0 LANGUAGES C CXX)

# ── Source files ─────────────────────────────────────────────────────────
target_sources(app PRIVATE
    src/main.c
    src/bench_b1_native.cpp
    src/bench_b1_wasm.c
    src/bench_b2_mpu.c
    src/bench_b3_audit.c
    src/bench_b4_mem.c
    src/bench_b5_ble.cpp
    # Minimal aiinfer native functions (PSRAM-arena, no capability gate)
    src/aiinfer_bench.cpp
    # AkiraOS runtime sources needed by B2 (sandbox) and B3 (audit)
    ${CMAKE_CURRENT_LIST_DIR}/../../src/runtime/security/sandbox.c
    # HMAC platform stub (only compiled when AKIRA_AUDIT_LOG_HMAC=y)
    src/akira_platform/audit_hmac.c
)

# ── Include directories ──────────────────────────────────────────────────
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../src/api
    ${CMAKE_CURRENT_LIST_DIR}/../../src/runtime
    ${CMAKE_CURRENT_LIST_DIR}/../../src/runtime/security
    # TFLM headers
    ${CMAKE_CURRENT_LIST_DIR}/../../modules/tflite-micro
    ${CMAKE_CURRENT_LIST_DIR}/../../modules/tflite-micro/third_party_static/flatbuffers/include
    ${CMAKE_CURRENT_LIST_DIR}/../../modules/tflite-micro/third_party_static/gemmlowp
    # WAMR headers
    ${CMAKE_CURRENT_LIST_DIR}/../../modules/wasm-micro-runtime/core/iwasm/include
    # hello_world_float model byte array
    ${CMAKE_CURRENT_LIST_DIR}/../../AkiraSDK/wasm_apps/aiinfer_test
)

# ── Mirror main CMakeLists: add AkiraOS TFLM submodule when AIINFER=y ───
if(CONFIG_AKIRA_AIINFER)
    enable_language(CXX)
    set(TFLM_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../modules/tflite-micro")
    if(CONFIG_AKIRA_TFLITE_MICRO AND EXISTS "${TFLM_ROOT_DIR}/CMakeLists.txt")
        add_subdirectory(${TFLM_ROOT_DIR} tflm_build EXCLUDE_FROM_ALL)
    endif()
    if(CONFIG_AKIRA_TFLITE_ESPNN AND CONFIG_SOC_SERIES_ESP32S3)
        target_compile_definitions(app PRIVATE TFLM_USE_ESPNN=1)
    endif()
    target_compile_definitions(app PRIVATE
        AKIRA_AIINFER_ARENA_KB=${CONFIG_AKIRA_AIINFER_ARENA_KB}
        AKIRA_AIINFER_MAX_HANDLES=${CONFIG_AKIRA_AIINFER_MAX_HANDLES}
    )
    if(TARGET tensorflow-lite-micro)
        target_link_libraries(app PRIVATE tensorflow-lite-micro)
    endif()
endif()

# ── WAMR build (mirroring main CMakeLists, simplified for bench) ────────
if(CONFIG_AKIRA_WASM_RUNTIME)
    set(WAMR_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../modules/wasm-micro-runtime")
    set(WAMR_BUILD_PLATFORM  "zephyr")
    # Detect Xtensa toolchain (ESP32-S3 → XTENSA_32)
    string(TOLOWER "${CMAKE_C_COMPILER}" _bench_cc)
    if(_bench_cc MATCHES "xtensa")
        set(WAMR_BUILD_TARGET "XTENSA_32")
    elseif(_bench_cc MATCHES "arm" OR _bench_cc MATCHES "arm.*eabi")
        set(WAMR_BUILD_TARGET "THUMB_V7")
    elseif(_bench_cc MATCHES "riscv")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(WAMR_BUILD_TARGET "RISCV64")
        else()
            set(WAMR_BUILD_TARGET "RISCV32")
        endif()
    else()
        set(WAMR_BUILD_TARGET "X86_32")
    endif()
    set(WAMR_BUILD_INTERP           1)
    set(WAMR_BUILD_MINI_LOADER      1)
    set(WAMR_BUILD_AOT              0)
    set(WAMR_BUILD_LIBC_BUILTIN     1)
    set(WAMR_BUILD_LIBC_WASI        0)
    set(WAMR_BUILD_GLOBAL_HEAP_POOL 0)
    set(WAMR_BUILD_REF_TYPES        1)
    include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
    target_sources(app PRIVATE ${WAMR_RUNTIME_LIB_SOURCE})
    target_include_directories(app PRIVATE
        ${WAMR_ROOT_DIR}/core/iwasm/include
        ${WAMR_ROOT_DIR}/core/shared
        ${WAMR_ROOT_DIR}/core/deps
    )
    # Minimal akira_runtime wrapper (no full AkiraOS dep chain)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src/akira_runtime_bench.c
    )
endif()

# ── WASM variant selection ───────────────────────────────────────────────
if(CONFIG_BENCH_WASM_NOGUARD)
    target_compile_definitions(app PRIVATE BENCH_WASM_VARIANT_NOGUARD=1)
endif()

# ── Linker symbols for TFLM section size (B4) ───────────────────────────
zephyr_linker_sources(SECTIONS
    ${CMAKE_CURRENT_LIST_DIR}/tflm_section.ld
)

# ── C++ standard ────────────────────────────────────────────────────────
set_property(TARGET app PROPERTY CXX_STANDARD 14)
