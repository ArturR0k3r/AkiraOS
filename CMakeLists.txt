# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# ===== WAMR Configuration Override =====
# Set WAMR libc configuration BEFORE find_package loads Zephyr modules.
# For embedded targets (XTENSA, ARM, RISC-V), use builtin libc since WASI libc
# requires sys/random.h which doesn't exist on embedded systems.
# This must be done as a cache variable so it persists through module loading.

# These will be overridden by our config in Zephyr module processing,
# but we set them early to influence CMake's cache
set(WAMR_BUILD_LIBC_BUILTIN 1 CACHE BOOL "" FORCE)
set(WAMR_BUILD_LIBC_WASI 0 CACHE BOOL "" FORCE)

# Force disable Kconfig warnings
set(KCONFIG_WARN_UNDEF OFF CACHE BOOL "Disable Kconfig warnings" FORCE)
set(KCONFIG_WERROR OFF CACHE BOOL "Disable Kconfig warnings as errors" FORCE)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(akira_os VERSION 1.2.3)

# Add AkiraOS include directory to all targets for sys/random.h shim
include_directories(BEFORE SYSTEM ${CMAKE_CURRENT_LIST_DIR}/include)

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
# Note: OCRE is now integrated as a Zephyr module and will be automatically
# built through the module system. The zephyr/CMakeLists.txt handles all
# OCRE library compilation (OcreCore, OcrePlatform, OcreRuntime, etc.)
# No need to manually add sources here - Zephyr's module system handles it.

# Akira Module System (Core functionality, not third-party)
if(CONFIG_AKIRA_MODULE)
    add_subdirectory(src/akira_modules)
endif()

# Add source files
target_sources(app PRIVATE
    src/main.c
    
    # Core drivers and shell
    src/drivers/display_ili9341.c
    src/drivers/fonts.c
    src/drivers/font_data.c
    src/drivers/platform_hal.c
    src/drivers/driver_registry.c
    src/shell/akira_shell.c
    src/shell/shell_display.c
    src/settings/settings.c
)

# ===== v2.0 Modular Architecture Files =====

# API Layer (WASM exports)
target_sources(app PRIVATE
    src/akira/akira_native_exports.c
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

# ===== Core Libraries =====
# Shared utilities used across modules
target_sources(app PRIVATE
    src/lib/path_utils.c
    src/lib/error_codes.c
)

# ===== Akira Core (kernel, HAL) =====
target_sources(app PRIVATE
    src/akira/init.c
    src/akira/kernel/service.c
    src/akira/kernel/event.c
    src/akira/kernel/process.c
    src/akira/kernel/memory.c
    src/akira/kernel/timer.c
    src/akira/kernel/psram.c
    src/akira/hal/hal.c
)

# Akira shell commands
if(CONFIG_SHELL)
    target_sources(app PRIVATE src/akira/shell.c)
    if(CONFIG_AKIRA_HID)
        target_sources(app PRIVATE src/shell/hid_shell.c)
    endif()
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

# Legacy embedded web UI (optional — add stub when disabled)
if(CONFIG_AKIRA_EMBEDDED_WEB)
    target_sources(app PRIVATE src/OTA/web_server.c)
else()
    target_sources(app PRIVATE src/OTA/web_server_stub.c)
endif()

# ===== OTA Transport Layer =====
if(CONFIG_AKIRA_OTA)
    target_sources(app PRIVATE src/OTA/ota_transport.c)
    
    if(CONFIG_AKIRA_OTA_HTTP)
        target_sources(app PRIVATE src/OTA/transports/ota_http.c)
    endif()
    
    if(CONFIG_AKIRA_OTA_BLE)
        target_sources(app PRIVATE src/OTA/transports/ota_ble.c)
    endif()
    
    if(CONFIG_AKIRA_OTA_USB)
        target_sources(app PRIVATE src/OTA/transports/ota_usb.c)
    endif()
    
    if(CONFIG_AKIRA_OTA_CLOUD)
        target_sources(app PRIVATE src/OTA/transports/ota_cloud.c)
    endif()
endif()

# ===== Connectivity Layer =====

# WiFi uses Zephyr's native stack - no wrapper needed

# HID Manager (when enabled)
if(CONFIG_AKIRA_HID)
    target_sources(app PRIVATE src/connectivity/hid/hid_manager.c)
    
    # HID Simulation (native_sim only)
    if(CONFIG_AKIRA_HID_SIM)
        target_sources(app PRIVATE src/connectivity/hid/hid_sim.c)
    endif()
endif()

# HTTP Server
if(CONFIG_AKIRA_HTTP_SERVER)
    target_sources(app PRIVATE src/connectivity/http/http_server.c)
endif()

# Bluetooth HID (conditional)
if(CONFIG_AKIRA_BT_HID)
    target_sources(app PRIVATE
        src/connectivity/bluetooth/bt_manager.c
        src/connectivity/bluetooth/bt_hid.c
    )
    if(CONFIG_AKIRA_BT_ECHO)
        target_sources(app PRIVATE src/connectivity/bluetooth/bt_echo.c)
    endif()
endif()

# USB HID (conditional)
if(CONFIG_AKIRA_USB_HID)
    target_sources(app PRIVATE
        src/connectivity/usb/usb_manager.c
        src/connectivity/usb/usb_hid.c
    )
endif()

# ===== Client Connectivity =====

# WebSocket Client
if(CONFIG_AKIRA_WS_CLIENT)
    target_sources(app PRIVATE src/connectivity/client/ws_client.c)
endif()

# CoAP Client
if(CONFIG_AKIRA_COAP_CLIENT)
    target_sources(app PRIVATE src/connectivity/client/coap_client.c)
endif()

# ===== Cloud Connectivity (Unified) =====

if(CONFIG_AKIRA_CLOUD_CLIENT)
    target_sources(app PRIVATE
        src/connectivity/cloud/cloud_protocol.c
        src/connectivity/cloud/cloud_client.c
    )
    
    # App handler (WASM app download/update)
    if(CONFIG_AKIRA_CLOUD_APP_HANDLER)
        target_sources(app PRIVATE src/connectivity/cloud/cloud_app_handler.c)
    endif()
    
    # OTA handler (firmware updates)
    if(CONFIG_AKIRA_CLOUD_OTA_HANDLER)
        target_sources(app PRIVATE src/connectivity/cloud/cloud_ota_handler.c)
    endif()
endif()

# Services (OCRE/WASM runtime wrapper)
target_sources(app PRIVATE
    src/services/akira_runtime.c
)

# Filesystem Manager (MUST be before App Manager)
target_sources(app PRIVATE src/storage/fs_manager.c)

# App Manager
if(CONFIG_AKIRA_APP_MANAGER)
    target_sources(app PRIVATE src/services/app_manager.c)
endif()

# Storage drivers for app sources
if(CONFIG_AKIRA_APP_SOURCE_SD)
    target_sources(app PRIVATE src/connectivity/storage/sd_manager.c)
endif()

if(CONFIG_AKIRA_APP_SOURCE_USB)
    target_sources(app PRIVATE src/connectivity/storage/usb_storage.c)
endif()

# BLE app transfer
if(CONFIG_AKIRA_APP_SOURCE_BLE)
    target_sources(app PRIVATE src/connectivity/bluetooth/bt_app_transfer.c)
endif()

# Include directories
target_include_directories(app PRIVATE
    src/
    src/akira/
    src/api/
    src/apps/
    src/connectivity/
    src/connectivity/bluetooth/
    src/connectivity/client/
    src/connectivity/cloud/
    src/connectivity/hid/
    src/connectivity/http/
    src/connectivity/usb/
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
    src/storage/
    src/ui/
    # OCRE headers - use include subdirectory to get public headers
    ../ocre/src/ocre/include
    ../ocre/src/runtime/include
    # WAMR headers needed for native exports
    ../ocre/wasm-micro-runtime/core/iwasm/include
    ../ocre/wasm-micro-runtime/core/shared/utils
)

# SDL2 simulator uses external viewer (tools/akira_viewer)
# No need to build SDL2 code within Zephyr build system
# if(CONFIG_BOARD_NATIVE_SIM)
#     add_subdirectory(src/drivers/sim)
# endif()

# Compile definitions
target_compile_definitions(app PRIVATE
    AKIRA_VERSION_MAJOR=1
    AKIRA_VERSION_MINOR=2
    AKIRA_VERSION_PATCH=3
)

# Link OCRE libraries if OCRE is enabled
# The Zephyr module system should handle this, but we add explicit linking
# to ensure the OCRE libraries are linked
if(CONFIG_OCRE)
    # Since Zephyr's module system isn't properly exporting OCRE targets,
    # we need to directly link against the full library paths or object files
    # Find the OCRE libraries that were built
    find_library(OCRECORE_LIB NAMES libOcreCore OcreCore PATHS "${CMAKE_BINARY_DIR}/modules/ocre/ocre-core" NO_DEFAULT_PATH)
    find_library(OCREPLATFORM_LIB NAMES libOcrePlatform OcrePlatform PATHS "${CMAKE_BINARY_DIR}/modules/ocre/ocre-zephyr" NO_DEFAULT_PATH)
    find_library(OCRERUNTIMEWAMR_LIB NAMES libOcreRuntimeWamr OcreRuntimeWamr PATHS "${CMAKE_BINARY_DIR}/modules/ocre/ocre-runtime-wamr-wasip1" NO_DEFAULT_PATH)
    find_library(OCRERUNTIMEAPI_LIB NAMES libOcreRuntimeAPI OcreRuntimeAPI PATHS "${CMAKE_BINARY_DIR}/modules/ocre/ocre-runtime-wamr-wasip1/ocre_api" NO_DEFAULT_PATH)
    find_library(VMLIB_LIB NAMES libvmlib vmlib PATHS "${CMAKE_BINARY_DIR}/modules/ocre" NO_DEFAULT_PATH)
    
    # Link them if found
    if(OCRECORE_LIB)
        target_link_libraries(app PRIVATE "${OCRECORE_LIB}")
    endif()
    if(OCREPLATFORM_LIB)
        target_link_libraries(app PRIVATE "${OCREPLATFORM_LIB}")
    endif()
    if(OCRERUNTIMEWAMR_LIB)
        target_link_libraries(app PRIVATE "${OCRERUNTIMEWAMR_LIB}")
    endif()
    if(OCRERUNTIMEAPI_LIB)
        target_link_libraries(app PRIVATE "${OCRERUNTIMEAPI_LIB}")
    endif()
    if(VMLIB_LIB)
        target_link_libraries(app PRIVATE "${VMLIB_LIB}")
    endif()
    
    if(CONFIG_OCRE_SHELL)
        find_library(OCRESHELL_LIB NAMES libOcreShell OcreShell PATHS "${CMAKE_BINARY_DIR}/modules/ocre/ocre-shell" NO_DEFAULT_PATH)
        if(OCRESHELL_LIB)
            target_link_libraries(app PRIVATE "${OCRESHELL_LIB}")
        endif()
    endif()
endif()