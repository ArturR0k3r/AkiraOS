# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Force disable Kconfig warnings
set(KCONFIG_WARN_UNDEF OFF CACHE BOOL "Disable Kconfig warnings" FORCE)
set(KCONFIG_WERROR OFF CACHE BOOL "Disable Kconfig warnings as errors" FORCE)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(akira_os VERSION 1.0.0)


# Set default board
set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/boards)
# set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/akira.overlay)  # Not needed, handled by board overlay

# Include WASM Micro Runtime if available
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/modules/wamr)
    add_subdirectory(modules/wamr/product/zephyr)
    target_compile_definitions(app PRIVATE CONFIG_WAMR_ENABLE=1)
endif()

# OCRE Integration
set(OCRE_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../ocre")
if(EXISTS "${OCRE_ROOT_DIR}/src")
    include("${CMAKE_CURRENT_LIST_DIR}/modules/ocre/ocre.cmake")
    target_sources(app PRIVATE ${ocre_component_sources} ${ocre_lib_sources})
    target_include_directories(app PRIVATE
        ${OCRE_ROOT_DIR}/src/
        ${OCRE_ROOT_DIR}/src/ocre
        ${OCRE_ROOT_DIR}/src/shared/platform
    )
    target_compile_definitions(app PRIVATE CONFIG_OCRE_ENABLE=1)
endif()

# Akira Module System (Core functionality, not third-party)
if(CONFIG_AKIRA_MODULE)
    add_subdirectory(src/akira_modules)
endif()

# Add source files
target_sources(app PRIVATE
    src/main.c
    src/drivers/display_ili9341.c
    src/drivers/fonts.c
    src/drivers/font_data.c
    src/drivers/akira_hal.c
    src/shell/akira_shell.c
    src/settings/settings.c
    src/OTA/web_server.c
)

# Conditional OTA manager based on flash availability
if(CONFIG_FLASH_MAP AND CONFIG_BOOTLOADER_MCUBOOT)
    target_sources(app PRIVATE src/OTA/ota_manager.c)
else()
    target_sources(app PRIVATE src/OTA/ota_manager_stub.c)
endif()

# Include directories
target_include_directories(app PRIVATE
    src/
    src/drivers/
    src/runtime/
    src/apps/
    src/ui/
    src/lib/
    src/OTA/
    src/settings/
    src/shell/
    src/drivers/
    ${CMAKE_CURRENT_LIST_DIR}/modules/wasm-micro-runtime/core/iwasm/include
    ${CMAKE_CURRENT_LIST_DIR}/modules/wasm-micro-runtime/core/shared/utils
)

# SDL2 simulator uses external viewer (tools/akira_viewer)
# No need to build SDL2 code within Zephyr build system
# if(CONFIG_BOARD_NATIVE_SIM)
#     add_subdirectory(src/drivers/sim)
# endif()

# Compile definitions
target_compile_definitions(app PRIVATE
    AKIRA_VERSION_MAJOR=1
    AKIRA_VERSION_MINOR=0
    AKIRA_VERSION_PATCH=0
)