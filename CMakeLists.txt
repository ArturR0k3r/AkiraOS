# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Force disable Kconfig warnings
set(KCONFIG_WARN_UNDEF OFF CACHE BOOL "Disable Kconfig warnings" FORCE)
set(KCONFIG_WERROR OFF CACHE BOOL "Disable Kconfig warnings as errors" FORCE)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(akira_os VERSION 1.0.0)


# Set default board
set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/boards)
set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/akira.overlay)

# Include WASM Micro Runtime if available
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/modules/wamr)
    add_subdirectory(modules/wamr/product/zephyr)
    target_compile_definitions(app PRIVATE CONFIG_WAMR_ENABLE=1)
endif()

# Add source files
target_sources(app PRIVATE
    src/main.c
    src/drivers/display_ili9341.c
)

# Include directories
target_include_directories(app PRIVATE
    src/
    src/drivers/
    src/runtime/
    src/apps/
    src/ui/
    src/lib/
)   

# Compile definitions
target_compile_definitions(app PRIVATE
    AKIRA_VERSION_MAJOR=1
    AKIRA_VERSION_MINOR=0
    AKIRA_VERSION_PATCH=0
)