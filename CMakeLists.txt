# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Force disable Kconfig warnings
set(KCONFIG_WARN_UNDEF OFF CACHE BOOL "Disable Kconfig warnings" FORCE)
set(KCONFIG_WERROR OFF CACHE BOOL "Disable Kconfig warnings as errors" FORCE)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(akira_os VERSION 1.2.3)


# Set default board
set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/boards)
# set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/akira.overlay)  # Not needed, handled by board overlay



# ===== WAMR Integration =====
set(WAMR_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/modules/wasm-micro-runtime")

if(CONFIG_AKIRA_WASM_RUNTIME)
    set(WAMR_BUILD_PLATFORM "zephyr")
    # Choose an appropriate WAMR build target for the current toolchain/board
    # Common targets: X86_32, X86_64, ARM_32, AARCH64, XTENSA_32, RISCV32, RISCV64
    if(NOT DEFINED WAMR_BUILD_TARGET)
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _proc)
        if(_proc MATCHES "x86_64|amd64")
            set(WAMR_BUILD_TARGET "X86_64" CACHE STRING "WAMR build target")
        elseif(_proc MATCHES "x86|i686|i386")
            set(WAMR_BUILD_TARGET "X86_32")
        elseif(_proc MATCHES "arm64|aarch64")
            set(WAMR_BUILD_TARGET "AARCH64")
        elseif(_proc MATCHES "arm|armv7")
            # For MCU Zephyr targets (Cortex-M / Thumb ISA) prefer THUMB variant so the
            # correct thumb asm implementation is selected; fallback to ARM_32 for non-MCU
            if (CMAKE_SYSTEM_NAME STREQUAL "Zephyr")
                set(WAMR_BUILD_TARGET "THUMB_V7")
            else()
                set(WAMR_BUILD_TARGET "ARM_32")
            endif()
        elseif(_proc MATCHES "xtensa")
            set(WAMR_BUILD_TARGET "XTENSA_32")
        elseif(_proc MATCHES "riscv")
            # Choose 32/64 based on pointer size
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(WAMR_BUILD_TARGET "RISCV64")
            else()
                set(WAMR_BUILD_TARGET "RISCV32")
            endif()
        else()
            # Fallback: try to infer from compiler executable if available
            string(TOLOWER "${CMAKE_C_COMPILER}" _cc)
            if(_cc MATCHES "xtensa")
                set(WAMR_BUILD_TARGET "XTENSA_32")
            elseif(_cc MATCHES "arm" OR _cc MATCHES "arm.*eabi" OR _cc MATCHES "arm-")
                # Cross-compiler for ARM MCU (Cortex-M): prefer THUMB target for MCU/Thumb ISA
                set(WAMR_BUILD_TARGET "THUMB_V7")
            elseif(_cc MATCHES "riscv")
                if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                    set(WAMR_BUILD_TARGET "RISCV64")
                else()
                    set(WAMR_BUILD_TARGET "RISCV32")
                endif()
            else()
                # Fall back to X86_32 for native_sim / host builds
                set(WAMR_BUILD_TARGET "X86_32")
            endif()
        endif()
    endif()

    set(WAMR_BUILD_INTERP 1)
    # Disable AOT for embedded MCU targets (Thumb/ARM) where the host-side AOT toolchain
    # or LLVM cross-backend may not be available. Enable AOT for host builds.
    if(DEFINED WAMR_BUILD_TARGET AND (WAMR_BUILD_TARGET MATCHES "^THUMB" OR WAMR_BUILD_TARGET MATCHES "^ARM"))
        set(WAMR_BUILD_AOT 0)
    else()
        set(WAMR_BUILD_AOT 1)
    endif()
    set(WAMR_BUILD_LIBC_BUILTIN 1)

    # Post-processing: if WAMR chose generic ARM_32 but we are using an MCU toolchain,
    # prefer a THUMB target so the correct thumb asm is selected.
    if(DEFINED WAMR_BUILD_TARGET AND WAMR_BUILD_TARGET STREQUAL "ARM_32")
        string(TOLOWER "${CMAKE_C_COMPILER}" _cc)
        if(_cc MATCHES "arm" OR _cc MATCHES "arm.*eabi" OR _cc MATCHES "arm-")
            message(STATUS "Mapping WAMR target ARM_32 -> THUMB_V7 for MCU toolchain detected")
            set(WAMR_BUILD_TARGET "THUMB_V7" CACHE STRING "WAMR build target" FORCE)
        endif()
    endif()

    set(WAMR_BUILD_LIBC_WASI 0)
    set(WAMR_BUILD_GLOBAL_HEAP_POOL 1)
    set(WAMR_BUILD_GLOBAL_HEAP_SIZE 131072)
    include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
    target_sources(app PRIVATE ${WAMR_RUNTIME_LIB_SOURCE})
    target_include_directories(app PRIVATE
        ${WAMR_ROOT_DIR}/core/iwasm/include
        ${WAMR_ROOT_DIR}/core/shared
        ${WAMR_ROOT_DIR}/core/deps
    )
    # Modular WASM API layer
    target_sources(app PRIVATE
        src/runtime/modules/akira_display_module.c
        src/runtime/modules/akira_log_module.c
        src/runtime/modules/akira_time_module.c
    )
    target_include_directories(app PRIVATE
        include/modules/
    )
    message(STATUS "WAMR enabled: modular WASM API layer will be built!")
else()
    message(WARNING "WASM support disabled - runtime will use stub implementations. To enable: set CONFIG_AKIRA_WASM_RUNTIME and ensure WAMR is integrated.")
endif()

# Akira Module System removed for Minimalist Architecture
# NOTE: akira_modules are no longer part of the core build. Keep local modules
# out-of-tree and integrate them separately if needed.

# Add source files
target_sources(app PRIVATE
    src/main.c
    
    # Core drivers and shell
    src/drivers/display/display_ili9341.c
    src/drivers/display/fonts.c
    src/drivers/display/font_data.c
    src/drivers/platform_hal.c
    src/drivers/psram.c
    src/drivers/driver_registry.c
    src/shell/akira_shell.c
    src/shell/shell_display.c
    src/settings/settings.c
)

# ===== v2.0 Modular Architecture Files =====

# API Layer (WASM exports) - now modular, see modules above

# Security Layer
# Capability and signing implementations live under src/runtime/security
target_sources(app PRIVATE
    src/runtime/security/capability.c
    src/runtime/security/app_signing.c
)

# Power Management
target_sources(app PRIVATE
    src/drivers/power/power_manager.c
)

# IPC Layer (now implemented internally in the runtime; no external sources)
# message_bus and shared_memory are provided by src/runtime and registered at runtime init.


# Resource Management (managed internally or not present in minimal build)
# resource_manager and scheduler are implemented elsewhere or intentionally omitted for this build.


# UI Framework
target_sources(app PRIVATE
    src/ui/ui_framework.c
)

# App Loader/Manager are implemented under src/runtime (loader/ and manager/) — included above

# ===== Core Libraries =====
# Shared utilities used across modules
target_sources(app PRIVATE
    src/lib/path_utils.c
    src/lib/error_codes.c
    src/lib/simple_json.c
)

# ===== Akira Core (kernel, HAL) =====
# Core kernel components are provided by Zephyr or are intentionally omitted in
# the Minimalist Architecture. Required helpers like PSRAM and platform HAL are
# provided via src/drivers/psram.c and src/drivers/platform_hal.c.
# Runtime core and related modules (One Module, One Functionality)
# We intentionally do NOT create per-module CMakeLists; keep a single list here
# per the architecture and to avoid scattered cmake files.

target_sources(app PRIVATE
    src/runtime/akira_runtime.c
    src/runtime/security.c
    src/runtime/app_loader/app_loader.c
    src/runtime/app_manager/app_manager.c
)

# Core API wrappers (must be stateless, call akira_security_check first)
target_sources(app PRIVATE
    src/api/akira_display_api.c
    src/api/akira_input_api.c
    src/api/akira_rf_api.c
    src/api/akira_sensor_api.c
)

# Keep connectivity and drivers as before (use module gating further below)

# Akira shell commands
if(CONFIG_SHELL)
    # Use existing shell sources under src/shell/
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

# RF Framework and Drivers (conditional on module gating)
if(CONFIG_AKIRA_MODULE_RF)
    if(CONFIG_AKIRA_RF_FRAMEWORK)
        target_sources(app PRIVATE src/drivers/rf_framework.c)
    endif()

    if(CONFIG_AKIRA_LR1121)
        target_sources(app PRIVATE src/drivers/lr1121.c)
    endif()

    if(CONFIG_AKIRA_CC1101)
        target_sources(app PRIVATE src/drivers/cc1101.c)
    endif()
endif()

# Sensor Drivers (conditional on sensors module)
if(CONFIG_AKIRA_MODULE_SENSORS)
    if(CONFIG_AKIRA_BME280)
        target_sources(app PRIVATE src/drivers/bme280.c)
    endif()

    if(CONFIG_AKIRA_NRF24L01)
        target_sources(app PRIVATE src/drivers/nrf24l01.c)
    endif()

    if(CONFIG_AKIRA_LSM6DS3)
        target_sources(app PRIVATE src/drivers/lsm6ds3.c)
    endif()

    if(CONFIG_AKIRA_INA219)
        target_sources(app PRIVATE src/drivers/ina219.c)
    endif()
endif()

# Conditional OTA manager based on flash availability
if(CONFIG_FLASH_MAP AND CONFIG_BOOTLOADER_MCUBOOT)
    target_sources(app PRIVATE 
        src/connectivity/transport_interface.c
        src/connectivity/ota/ota_manager.c
    )
endif()

# Legacy embedded web UI (optional)
if(CONFIG_AKIRA_EMBEDDED_WEB)
    target_sources(app PRIVATE src/connectivity/ota/web_server.c)
endif()

# ===== OTA Transport Layer =====
# Only add OTA transports when OTA management is available (i.e., flash + mcuboot)
if(CONFIG_AKIRA_OTA AND CONFIG_FLASH_MAP AND CONFIG_BOOTLOADER_MCUBOOT)
    
    if(CONFIG_AKIRA_OTA_HTTP)
        target_sources(app PRIVATE src/connectivity/ota/transports/ota_http.c)
    endif()
    
    if(CONFIG_AKIRA_OTA_BLE)
        target_sources(app PRIVATE src/connectivity/ota/transports/ota_ble.c)
    endif()
    
    if(CONFIG_AKIRA_OTA_USB)
        target_sources(app PRIVATE src/connectivity/ota/transports/ota_usb.c)
    endif()
    
    if(CONFIG_AKIRA_OTA_CLOUD)
        target_sources(app PRIVATE src/connectivity/ota/transports/ota_cloud.c)
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

# Bluetooth HID (conditional - build when BT HID is enabled)
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

# Services (Unified WASM runtime wrapper)
# Using single unified runtime implementation in src/runtime
# Runtime and native API registration happen in src/runtime/akira_runtime.c
# (No external akira_native_api.c is built.)

# Filesystem Manager (MUST be before App Manager)
target_sources(app PRIVATE src/storage/fs_manager.c)



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
    src/runtime/
    src/security/
    src/services/
    src/settings/
    src/shell/
    src/storage/
    src/ui/
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