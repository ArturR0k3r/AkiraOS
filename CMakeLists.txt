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

# REMOVED - Using OCRE's WAMR instead
# # Include WASM Micro Runtime if available
# if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/modules/wamr)
#     add_subdirectory(modules/wamr/product/zephyr)
#     target_compile_definitions(app PRIVATE CONFIG_WAMR_ENABLE=1)
# endif()

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

# ===== v2.0 Modular Architecture Files =====

# API Layer (WASM exports)
target_sources(app PRIVATE
    src/api/akira_display_api.c
    src/api/akira_input_api.c
    src/api/akira_rf_api.c
    src/api/akira_sensor_api.c
    src/api/akira_storage_api.c
    src/api/akira_network_api.c
    src/api/akira_system_api.c
)

# Security Layer
target_sources(app PRIVATE
    src/security/capability.c
    src/security/app_signing.c
)

# Power Management
target_sources(app PRIVATE
    src/power/power_manager.c
)

# IPC Layer
target_sources(app PRIVATE
    src/ipc/message_bus.c
    src/ipc/shared_memory.c
)

# Resource Management
target_sources(app PRIVATE
    src/resource/resource_manager.c
    src/resource/scheduler.c
)

# UI Framework
target_sources(app PRIVATE
    src/ui/ui_framework.c
)

# App Loader
target_sources(app PRIVATE
    src/apps/app_loader.c
)

# ===== Akira Core (kernel, HAL) =====
target_sources(app PRIVATE
    src/akira/init.c
    src/akira/kernel/service.c
    src/akira/kernel/event.c
    src/akira/kernel/process.c
    src/akira/kernel/memory.c
    src/akira/kernel/timer.c
    src/akira/hal/hal.c
)

# Akira shell commands
if(CONFIG_SHELL)
    target_sources(app PRIVATE src/akira/shell.c)
endif()

# New Display Drivers (conditional)
if(CONFIG_AKIRA_SSD1306)
    target_sources(app PRIVATE src/drivers/ssd1306.c)
endif()

if(CONFIG_AKIRA_ST7789)
    target_sources(app PRIVATE src/drivers/st7789.c)
endif()

# RF Framework and Drivers (conditional)
if(CONFIG_AKIRA_RF_FRAMEWORK)
    target_sources(app PRIVATE src/drivers/rf_framework.c)
endif()

if(CONFIG_AKIRA_LR1121)
    target_sources(app PRIVATE src/drivers/lr1121.c)
endif()

if(CONFIG_AKIRA_CC1101)
    target_sources(app PRIVATE src/drivers/cc1101.c)
endif()

# Sensor Drivers (conditional)
if(CONFIG_AKIRA_BME280)
    target_sources(app PRIVATE src/drivers/bme280.c)
endif()

# Conditional sensor drivers
if(CONFIG_AKIRA_NRF24L01)
    target_sources(app PRIVATE src/drivers/nrf24l01.c)
endif()

if(CONFIG_AKIRA_LSM6DS3)
    target_sources(app PRIVATE src/drivers/lsm6ds3.c)
endif()

if(CONFIG_AKIRA_INA219)
    target_sources(app PRIVATE src/drivers/ina219.c)
endif()

# Conditional OTA manager based on flash availability
if(CONFIG_FLASH_MAP AND CONFIG_BOOTLOADER_MCUBOOT)
    target_sources(app PRIVATE src/OTA/ota_manager.c)
else()
    target_sources(app PRIVATE src/OTA/ota_manager_stub.c)
endif()

# Services (OCRE/WASM runtime wrappers)
target_sources(app PRIVATE
    src/services/ocre_runtime.c
    src/services/wasm_app_manager.c
)

# Bluetooth (conditional)
if(CONFIG_BT)
    target_sources(app PRIVATE src/bluetooth/bluetooth_manager.c)
endif()

# Include directories
target_include_directories(app PRIVATE
    src/
    src/akira/
    src/api/
    src/apps/
    src/bluetooth/
    src/drivers/
    src/ipc/
    src/lib/
    src/OTA/
    src/power/
    src/resource/
    src/security/
    src/services/
    src/settings/
    src/shell/
    src/ui/
    # REMOVED - OCRE provides WAMR includes
    # ${CMAKE_CURRENT_LIST_DIR}/modules/wasm-micro-runtime/core/iwasm/include
    # ${CMAKE_CURRENT_LIST_DIR}/modules/wasm-micro-runtime/core/shared/utils
)

# SDL2 simulator uses external viewer (tools/akira_viewer)
# No need to build SDL2 code within Zephyr build system
# if(CONFIG_BOARD_NATIVE_SIM)
#     add_subdirectory(src/drivers/sim)
# endif()

# Compile definitions
target_compile_definitions(app PRIVATE
    AKIRA_VERSION_MAJOR=2
    AKIRA_VERSION_MINOR=0
    AKIRA_VERSION_PATCH=0
)