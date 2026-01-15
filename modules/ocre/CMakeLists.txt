cmake_minimum_required(VERSION 3.20.0)

# OCRE Module CMakeLists.txt
# This file is used because ocre/zephyr/module.yml has cmake-ext: True
# which tells Zephyr to use this external CMakeLists.txt instead of OCRE's own.

message(STATUS "[OCRE Module] Processing CMakeLists.txt, CONFIG_OCRE=${CONFIG_OCRE}")

# Only build OCRE for the main application, not for MCUBoot
if(CONFIG_OCRE)
    message(STATUS "[OCRE Module] Building OCRE libraries (CONFIG_OCRE=y)")
    
    # Get the OCRE source directory (workspace root is 3 levels up from modules/ocre/)
    # AkiraOS/modules/ocre -> AkiraOS/modules -> AkiraOS -> Akira (workspace) -> ocre
    get_filename_component(OCRE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../ocre" ABSOLUTE)
    message(STATUS "[OCRE Module] OCRE_DIR=${OCRE_DIR}")
    
    set(WAMR_BUILD_PLATFORM "zephyr")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_C_STANDARD 99)
    set(CMAKE_CXX_STANDARD 17)
    
    # Set WAMR root directory before including wamr.cmake
    # wamr.cmake uses CMAKE_CURRENT_SOURCE_DIR which would be wrong
    set(WAMR_ROOT_DIR ${OCRE_DIR}/../wasm-micro-runtime)
    
    # Include WAMR configuration (using OCRE_DIR for correct path)
    # Save and restore CMAKE_CURRENT_SOURCE_DIR equivalent
    set(SAVED_CURRENT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    set(CMAKE_CURRENT_SOURCE_DIR ${OCRE_DIR}/zephyr)
    include(${OCRE_DIR}/zephyr/wamr.cmake)
    set(CMAKE_CURRENT_SOURCE_DIR ${SAVED_CURRENT_SOURCE_DIR})
    
    # Add OCRE subdirectories
    add_subdirectory(${OCRE_DIR}/src/ocre ocre-core)
    add_subdirectory(${OCRE_DIR}/src/runtime ocre-runtime)
    add_subdirectory(${OCRE_DIR}/src/runtime/wamr-wasip1 ocre-runtime-wamr-wasip1)
    add_subdirectory(${OCRE_DIR}/src/platform/zephyr ocre-zephyr)
    
    if(CONFIG_OCRE_SHELL)
        message(STATUS "[OCRE Module] Shell is enabled")
        add_subdirectory(${OCRE_DIR}/src/shell ocre-shell)
    endif()
    
    if(CONFIG_OCRE_STORAGE_PARTITION)
        include(${OCRE_DIR}/zephyr/storage_partition.cmake)
    endif()
else()
    message(STATUS "[OCRE Module] Skipping OCRE build (CONFIG_OCRE not set)")
endif()

message ("Ocre Module CMakeLists.txt")