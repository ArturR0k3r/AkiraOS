cmake_minimum_required(VERSION 3.20)
project(akira_sim C)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# WAMR — embed the same runtime used by the firmware
# ---------------------------------------------------------------------------
set(WAMR_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../modules/wasm-micro-runtime")

set(WAMR_BUILD_PLATFORM   "linux")
set(WAMR_BUILD_TARGET     "X86_64")
set(WAMR_BUILD_INTERP     1)
set(WAMR_BUILD_AOT        0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_REF_TYPES  1)
set(WAMR_BUILD_GLOBAL_HEAP_POOL 1)
set(WAMR_BUILD_GLOBAL_HEAP_SIZE 131072)

include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

# ---------------------------------------------------------------------------
# SDL2
# ---------------------------------------------------------------------------
find_package(SDL2 REQUIRED)

# ---------------------------------------------------------------------------
# Simulator sources
# ---------------------------------------------------------------------------
add_executable(akira_sim
    main.c
    host/sim_display.c
    host/sim_input.c
    host/sim_storage.c
    host/sim_log.c
    host/sim_time.c
    wamr/wamr_host.c
    ${WAMR_RUNTIME_LIB_SOURCE}
)

target_include_directories(akira_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/wamr
    ${WAMR_ROOT_DIR}/core/iwasm/include
    ${WAMR_ROOT_DIR}/core/shared
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(akira_sim PRIVATE
    ${SDL2_LIBRARIES}
    m
    pthread
    dl
)

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
target_compile_options(akira_sim PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -g
)

message(STATUS "AkiraConsole Simulator build configured")
message(STATUS "  WAMR: ${WAMR_ROOT_DIR}")
message(STATUS "  SDL2: ${SDL2_LIBRARIES}")
