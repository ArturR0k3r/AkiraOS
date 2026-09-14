# SPDX-License-Identifier: Apache-2.0
#
# CMake entry point of the akira-os Zephyr module (see zephyr/module.yml).
#
# Zephyr processes this file for every application in a workspace that
# contains akira-os. AkiraOS is built only when the application sets
# CONFIG_AKIRA_OS=y, so images that merely use this repository's boards
# (MCUboot, unrelated apps) are unaffected.
#
# An application uses AkiraOS by setting CONFIG_AKIRA_OS=y and linking the
# interface target that carries the AkiraOS include paths:
#
#   target_link_libraries(app PRIVATE akira_os)

if(NOT CONFIG_AKIRA_OS)
    return()
endif()

get_filename_component(AKIRA_OS_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
set(WAMR_ROOT_DIR ${ZEPHYR_WASM_MICRO_RUNTIME_MODULE_DIR})

# Include paths for AkiraOS sources and for applications that use AkiraOS.
add_library(akira_os INTERFACE)

# AkiraOS version from this module's VERSION file. Defined on the interface so
# a product application with its own VERSION still reports the AkiraOS version
# it was built against (src/akira.h keeps predefined AKIRA_VERSION_* values).
function(akira_os_define_version)
    set(VERSION_FILE ${AKIRA_OS_DIR}/VERSION)
    set(VERSION_TYPE AKIRA)
    include(${ZEPHYR_BASE}/cmake/modules/version.cmake)
    target_compile_definitions(akira_os INTERFACE
        AKIRA_VERSION_MAJOR=${AKIRA_VERSION_MAJOR}
        AKIRA_VERSION_MINOR=${AKIRA_VERSION_MINOR}
        AKIRA_VERSION_PATCH=${AKIRA_PATCHLEVEL}
        AKIRA_VERSION_STRING="${AKIRA_VERSION_STRING}"
    )
endfunction()
akira_os_define_version()

# Zephyr links its interface libraries (FS, ELMFAT, LITTLEFS, ...) into 'app'
# only, after every module and subsystem has been processed. AkiraOS sources
# live in module libraries, so give the akira_os interface the same set, with
# the same CONFIG_APP_LINK_WITH_<LIB> switches, once configuration is complete.
function(akira_os_link_zephyr_interface_libs)
    get_property(libs GLOBAL PROPERTY ZEPHYR_INTERFACE_LIBS)
    foreach(lib ${libs})
        string(TOUPPER ${lib} lib_upper)
        if(CONFIG_APP_LINK_WITH_${lib_upper})
            target_link_libraries(akira_os INTERFACE ${lib})
        endif()
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL akira_os_link_zephyr_interface_libs)

# Warn (development) or fail (CONFIG_AKIRA_RELEASE_BUILD=y) on insecure settings.
include(${AKIRA_OS_DIR}/cmake/akira_release_checks.cmake)

# Applications that compile a hand-picked subset of AkiraOS sources themselves
# (tests/, bench/) set AKIRA_OS_BUILD_LIBRARIES to OFF before find_package(Zephyr).
if(DEFINED AKIRA_OS_BUILD_LIBRARIES AND NOT AKIRA_OS_BUILD_LIBRARIES)
    return()
endif()

# ===== WAMR (wasm-micro-runtime west project) =====
if(CONFIG_AKIRA_WASM_RUNTIME AND NOT EXISTS "${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")
    message(FATAL_ERROR
        "CONFIG_AKIRA_WASM_RUNTIME=y but the wasm-micro-runtime west project was not found. "
        "Run: west update wasm-micro-runtime")
endif()

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
    # Mini-loader: skips redundant module-level validation already handled by
    # akira_runtime (SHA-256 integrity check + sandbox capability audit).
    # Reduces WAMR flash footprint by ~20-30 KB. Disable if loading untrusted
    # binaries directly from WAMR without AkiraOS's own security layer.
    set(WAMR_BUILD_MINI_LOADER 1)
    # Check user configuration for AOT support
    if(CONFIG_WAMR_AOT_SUPPORT)
        set(WAMR_BUILD_AOT 1)
        message(STATUS "WAMR AOT support: ENABLED (via CONFIG_WAMR_AOT_SUPPORT)")
        # ESP32-S3 with PSRAM: AOT code lives in PSRAM and is executed
        # through the instruction-bus alias.  The loader must write via
        # the data-bus mirror and flush caches before execution.
        if(CONFIG_SOC_SERIES_ESP32S3 AND CONFIG_ESP_SPIRAM)
            add_definitions(-DWASM_MEM_DUAL_BUS_MIRROR=1)
            message(STATUS "WAMR dual-bus mirror: ENABLED (PSRAM instruction cache)")
        endif()
    else()
        set(WAMR_BUILD_AOT 0)
        message(STATUS "WAMR AOT support: DISABLED (via CONFIG_WAMR_AOT_SUPPORT)")
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
    # The runtime initializes WAMR with Alloc_With_Allocator (see
    # akira_runtime_init(): wamr_malloc/free/realloc route to PSRAM-first
    # akira_malloc_buffer()). The WAMR global heap pool is therefore unused —
    # leaving it enabled reserves a static global_heap_buf[CONFIG_WAMR_HEAP_SIZE]
    # (256 KB) in DRAM .bss for nothing, overflowing dram0_0_seg once WiFi is on.
    # Keep it disabled so DRAM is not wasted; CONFIG_WAMR_HEAP_SIZE no longer
    # backs a static pool.
    set(WAMR_BUILD_GLOBAL_HEAP_POOL 0)
    # Enable reference-types support so WAMR can load WASM binaries produced by
    # WASI SDK 19+ / LLVM 17+.  Those toolchains emit element sections in the
    # extended multi-table format (non-zero segment-kind prefix) even when user
    # code itself does not use reference types.  Without this flag WAMR rejects
    # such binaries with "zero byte expected … reference types … disabled".
    set(WAMR_BUILD_REF_TYPES 1)
    include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
    # Match the WAMR MPU thread pool to MAX_CONTAINERS to save 8 KB of static DRAM.
    add_compile_definitions(BH_ZEPHYR_MPU_STACK_COUNT=${CONFIG_MAX_CONTAINERS})

    zephyr_library_named(akira_wamr)
    zephyr_library_sources(${WAMR_RUNTIME_LIB_SOURCE})
    zephyr_library_link_libraries(akira_os)

    target_include_directories(akira_os INTERFACE
        ${WAMR_ROOT_DIR}/core/iwasm/include
        ${WAMR_ROOT_DIR}/core/shared
        ${WAMR_ROOT_DIR}/core/deps
    )
    message(STATUS "WAMR enabled: modular WASM API layer will be built!")
else()
    message(WARNING "WASM support disabled - runtime will use stub implementations. To enable: set CONFIG_AKIRA_WASM_RUNTIME and ensure WAMR is integrated.")
endif()

# ===== AkiraOS core: runtime, services, drivers =====
zephyr_library_named(akira_os_core)
zephyr_library_link_libraries(akira_os)

target_include_directories(akira_os INTERFACE ${AKIRA_OS_DIR}/include/modules/)

if(CONFIG_AKIRA_WASM_RUNTIME)
    # Modular WASM API layer
    # NOTE: akira_log_module and akira_time_module register to separate namespaces
    # ("akira_log", "akira_time") for WASM apps using named imports.
    # Their akira_register_*_module() functions must be called in akira_runtime_init()
    # before wasm_runtime_full_init().
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/runtime/modules/akira_log_module.c
        ${AKIRA_OS_DIR}/src/runtime/modules/akira_time_module.c
    )
endif()

# Akira Module System removed for Minimalist Architecture
# NOTE: akira_modules are no longer part of the core build. Keep local modules
# out-of-tree and integrate them separately if needed.

# Add source files
zephyr_library_sources(
    ${AKIRA_OS_DIR}/src/akira_os.c
    ${AKIRA_OS_DIR}/src/akira_platform_stubs.c

    # Core drivers and shell

    ${AKIRA_OS_DIR}/src/drivers/platform_hal.c
    ${AKIRA_OS_DIR}/src/drivers/psram.c
    ${AKIRA_OS_DIR}/src/drivers/driver_registry.c
    ${AKIRA_OS_DIR}/src/shell/akira_shell.c
    ${AKIRA_OS_DIR}/src/settings/settings.c
)

# Display shell commands (only if display is available)
if(CONFIG_DISPLAY)
    zephyr_library_sources(
    ${AKIRA_OS_DIR}/src/shell/shell_display_cmds.c
    ${AKIRA_OS_DIR}/src/drivers/display/display_ili9341.c
    ${AKIRA_OS_DIR}/src/drivers/display/fonts.c
    ${AKIRA_OS_DIR}/src/drivers/display/font_data.c
    ${AKIRA_OS_DIR}/src/shell/shell_display.c)
endif()

# ===== v2.0 Modular Architecture Files =====

# API Layer (WASM exports) - now modular, see modules above

# Security Layer
# Capability, signing, sandbox, and runtime cache implementations
zephyr_library_sources(
    ${AKIRA_OS_DIR}/src/runtime/security/app_signing_v2.c
    ${AKIRA_OS_DIR}/src/runtime/security/sandbox.c
    ${AKIRA_OS_DIR}/src/runtime/runtime_cache.c
)

# Power Management
zephyr_library_sources(
    ${AKIRA_OS_DIR}/src/drivers/power/power_manager.c
)

# IPC Layer (now implemented internally in the runtime; no external sources)
# message_bus and shared_memory are provided by src/runtime and registered at runtime init.

# Resource Management (managed internally or not present in minimal build)
# resource_manager and scheduler are implemented elsewhere or intentionally omitted for this build.

# App Loader/Manager are implemented under src/runtime (loader/ and manager/) — included above

# ===== Core Libraries =====
# Shared utilities used across modules
zephyr_library_sources(
    ${AKIRA_OS_DIR}/src/lib/path_utils.c
    ${AKIRA_OS_DIR}/src/lib/error_codes.c
    ${AKIRA_OS_DIR}/src/lib/simple_json.c
    ${AKIRA_OS_DIR}/src/lib/mem_helper.c
    ${AKIRA_OS_DIR}/src/lib/akpkg.c
)

# ===== Akira Core (kernel, HAL) =====
# Core kernel components are provided by Zephyr or are intentionally omitted in
# the Minimalist Architecture. Required helpers like PSRAM and platform HAL are
# provided via src/drivers/psram.c and src/drivers/platform_hal.c.
# Runtime core and related modules (One Module, One Functionality)
# We intentionally do NOT create per-module CMakeLists; keep a single list here
# per the architecture and to avoid scattered cmake files.

zephyr_library_sources(
    ${AKIRA_OS_DIR}/src/runtime/akira_runtime.c
    ${AKIRA_OS_DIR}/src/runtime/security.c
    ${AKIRA_OS_DIR}/src/runtime/manifest_parser.c
    ${AKIRA_OS_DIR}/src/runtime/app_loader/app_loader.c
    ${AKIRA_OS_DIR}/src/runtime/app_manager/app_manager.c
    ${AKIRA_OS_DIR}/src/runtime/app_manager/app_events.c
    ${AKIRA_OS_DIR}/src/runtime/app_manager/app_cmd_bridge.c
)

# Core API wrappers (must be stateless, call akira_security_check first)
zephyr_library_sources(
    ${AKIRA_OS_DIR}/src/api/akira_memory_api.c
    ${AKIRA_OS_DIR}/src/api/akira_common_api.c
)

# Display API is only built if display support is enabled
if(CONFIG_DISPLAY)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/api/akira_display_api.c
    )
endif()

# WASM-dependent API wrappers (need wasm_export.h)
if(CONFIG_AKIRA_WASM_RUNTIME)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/api/akira_export_api.c
    )
endif()

if(CONFIG_GPIO)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/api/akira_gpio_api.c
    )
endif()

if(CONFIG_AKIRA_MODULE_RF)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_rf_api.c)
endif()

if(CONFIG_AKIRA_WASM_PYTHON)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_python_api.c)
endif()

if(CONFIG_AKIRA_WASM_API AND CONFIG_SENSOR)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_sensor_api.c)
endif()

if(CONFIG_AKIRA_WASM_TIMER)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_timer_api.c)
endif()

if(CONFIG_AKIRA_WASM_UART)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_uart_api.c)
endif()

if(CONFIG_AKIRA_WASM_I2C)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_i2c_api.c)
endif()

if(CONFIG_AKIRA_WASM_PWM)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_pwm_api.c)
endif()

if(CONFIG_AKIRA_WASM_ADC)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_adc_api.c)
endif()

if(CONFIG_AKIRA_WDT)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/wdt/akira_wdt.c)
endif()

if(CONFIG_AKIRA_WASM_WDT)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_wdt_api.c)
endif()

if(CONFIG_AKIRA_WASM_STORAGE)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_storage_api.c)
endif()

if(CONFIG_AKIRA_WASM_BLE)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_ble_api.c)
endif()

if(CONFIG_AKIRA_WASM_NET)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/api/akira_net_api.c
        ${AKIRA_OS_DIR}/src/connectivity/net/net_stream.c
    )
endif()

if(CONFIG_AKIRA_WASM_POWER)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_power_api.c)
endif()

if(CONFIG_AKIRA_WASM_SETTINGS)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_settings_api.c)
endif()

if(CONFIG_AKIRA_WASM_HID)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_hid_api.c)
endif()

if(CONFIG_AKIRA_SYSTEM_API)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_system_api.c)
endif()

if(CONFIG_AKIRA_WASM_LIFECYCLE)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_lifecycle_api.c)
endif()

if(CONFIG_AKIRA_WASM_IPC)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/api/akira_ipc_api.c
        ${AKIRA_OS_DIR}/src/runtime/akira_ipc.c
    )
endif()

# Track B — new WASM native APIs
if(CONFIG_AKIRA_WASM_FS)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_fs_api.c)
endif()

if(CONFIG_AKIRA_WASM_CRYPTO)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_crypto_api.c)
endif()

if(CONFIG_AKIRA_WASM_RTC)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_rtc_api.c)
endif()

if(CONFIG_AKIRA_WASM_OTA)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_ota_api.c)
endif()

# Track A — OTA extensions
if(CONFIG_AKIRA_BOOT_GUARD)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/runtime/akira_boot_guard.c)
endif()

if(CONFIG_AKIRA_OTA_DELTA)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/lib/akira_delta.c)
endif()

# ===== AkiraClaw — Edge AI Inference (TFLite Micro) =====
if(CONFIG_AKIRA_AIINFER)
    set(TFLM_ROOT_DIR ${ZEPHYR_TFLITE_MICRO_MODULE_DIR})

    if(CONFIG_AKIRA_TFLITE_MICRO)
        if(EXISTS "${TFLM_ROOT_DIR}/CMakeLists.txt")
            # Build TFLM as an interface library so we control compilation flags.
            add_subdirectory(${TFLM_ROOT_DIR} tflm_build EXCLUDE_FROM_ALL)
        else()
            message(FATAL_ERROR
                "CONFIG_AKIRA_TFLITE_MICRO=y but the tflite-micro west project was not found at "
                "${TFLM_ROOT_DIR}. Run: west update tflite-micro")
        endif()

        if(CONFIG_AKIRA_TFLITE_ESPNN AND CONFIG_SOC_SERIES_ESP32S3)
            zephyr_library_compile_definitions(TFLM_USE_ESPNN=1)
        endif()

        if(CONFIG_AKIRA_TFLITE_CMSIS_NN AND CONFIG_CPU_CORTEX_M)
            zephyr_library_compile_definitions(TFLM_USE_CMSIS_NN=1)
        endif()
    endif()

    zephyr_library_compile_definitions(
        AKIRA_AIINFER_ARENA_KB=${CONFIG_AKIRA_AIINFER_ARENA_KB}
        AKIRA_AIINFER_MAX_HANDLES=${CONFIG_AKIRA_AIINFER_MAX_HANDLES}
    )

    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_aiinfer_api.cpp)

    # No extra includes needed: CONFIG_EXTERNAL_LIBC=y on native_sim removes
    # -nostdinc so the host C++ STL is found automatically.

    if(TARGET tensorflow-lite-micro)
        zephyr_library_link_libraries(tensorflow-lite-micro)
    endif()
endif() # CONFIG_AKIRA_AIINFER

# Track D — DX: telemetry, panic handler, runtime shell commands
if(CONFIG_AKIRA_TELEMETRY)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/lib/akira_telemetry.c)
endif()

if(CONFIG_AKIRA_PANIC)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/lib/akira_panic.c)
endif()

# Keep connectivity and drivers as before (use module gating further below)

# Akira shell commands
if(CONFIG_SHELL)
    # Use existing shell sources under src/shell/
    if(CONFIG_AKIRA_HID)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/shell/hid_shell.c)
    endif()

    zephyr_library_sources(${AKIRA_OS_DIR}/src/shell/akira_runtime_cmds.c)

    if(CONFIG_AKIRA_MODULE_RF)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/shell/shell_rf.c)
    endif()
endif()

# New Display Drivers (conditional)
if(CONFIG_AKIRA_SSD1306)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/ssd1306.c)
endif()

# Hardware Display HAL (using Zephyr display subsystem)
if(CONFIG_DISPLAY)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/drivers/display/display_hal.c
    )
endif()

# RF Framework and Drivers (conditional on module gating)
if(CONFIG_AKIRA_MODULE_RF)
    if(CONFIG_AKIRA_LR1121)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/rf/lr1121.c)
    endif()

    if(CONFIG_AKIRA_CC1101)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/rf/cc1101.c)
    endif()

    if(CONFIG_AKIRA_CC1121)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/rf/cc1121.c)
    endif()

    if(CONFIG_AKIRA_LR2021)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/rf/lr2021.c)
    endif()

    if(CONFIG_AKIRA_NRF24L01)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/rf/nrf24l01.c)
    endif()
endif()

if(CONFIG_AKIRA_INA219)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/sensors/ina219.c)
endif()

if(CONFIG_AKIRA_LSM6DS3_DRIVER)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/akira_lsm6ds3.c)
endif()

if(CONFIG_AKIRA_ILI9341_DRIVER)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/display/akira_ili9341.c)
endif()

if(CONFIG_CHARGER_BQ25601)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/charger/bq25601.c)
endif()

if(CONFIG_FUEL_GAUGE_BQ28Z610)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/power/bq28z610.c)
endif()

if(CONFIG_AKIRA_ST25DV)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/nfc/st25dv.c)
endif()

if(CONFIG_AKIRA_SE050)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/secure_element/se050.c
                                ${AKIRA_OS_DIR}/src/drivers/secure_element/se050_transport.c)
endif()

if(CONFIG_AKIRA_PWM_DIAL)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/input/akira_pwm_dial.c)
endif()

# Conditional OTA manager based on flash availability
if(CONFIG_AKIRA_OTA AND CONFIG_FLASH_MAP AND CONFIG_BOOTLOADER_MCUBOOT)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/connectivity/transport_interface.c
        ${AKIRA_OS_DIR}/src/connectivity/ota/ota_manager.c
    )
endif()

# Shared buffer pool for connectivity (HTTP uploads, OTA, etc.)
if(CONFIG_AKIRA_HTTP_SERVER OR CONFIG_AKIRA_OTA)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/buf_pool.c)
endif()

# ===== OTA Transport Layer =====
# Only add OTA transports when OTA management is available (i.e., flash + mcuboot)
if(CONFIG_AKIRA_OTA AND CONFIG_FLASH_MAP AND CONFIG_BOOTLOADER_MCUBOOT)

    if(CONFIG_AKIRA_OTA_HTTP)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/ota/transports/ota_http.c)
    endif()

    if(CONFIG_AKIRA_OTA_BLE)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/ota/transports/ota_ble.c)
    endif()

    if(CONFIG_AKIRA_OTA_USB)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/ota/transports/ota_usb.c)
    endif()

    if(CONFIG_AKIRA_OTA_CLOUD)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/ota/transports/ota_cloud.c)
    endif()
endif()

# ===== Connectivity Layer =====

# WiFi uses Zephyr's native stack - no wrapper needed

# HID Manager (when enabled)
if(CONFIG_AKIRA_HID)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/hid/hid_manager.c)

    # HID Simulation (native_sim only)
    if(CONFIG_AKIRA_HID_SIM)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/hid/hid_sim.c)
    endif()
endif()

# HTTP Server — generic route-based server + API routes + shell commands
if(CONFIG_AKIRA_HTTP_SERVER)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/connectivity/http/http_server.c
        ${AKIRA_OS_DIR}/src/connectivity/http/http_routes.c
        ${AKIRA_OS_DIR}/src/shell/shell_http.c
    )
endif()

# Date/time shell command — always include when Zephyr shell is enabled
if(CONFIG_SHELL)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/shell/shell_date.c)
endif()

# Bluetooth manager — needed by both HID mode and BLE app API mode
if(CONFIG_BT OR CONFIG_AKIRA_WASM_BLE)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/bluetooth/bt_manager.c)
endif()

if(CONFIG_AKIRA_BT_HID)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/bluetooth/bt_hid.c)
endif()

if(CONFIG_AKIRA_BT_ECHO)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/bluetooth/bt_echo.c)
endif()

if(CONFIG_AKIRA_WASM_BLE)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/bluetooth/ble_app_service.c)
endif()

if(CONFIG_AKIRA_BT_COMPANION)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/bluetooth/companion_service.c)
endif()

if(CONFIG_AKIRA_USB)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/connectivity/usb/usb_manager.c
        ${AKIRA_OS_DIR}/src/connectivity/usb/usb_manager_shell.c
    )
endif()

if(CONFIG_AKIRA_USB_HID)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/connectivity/usb/usb_hid.c
    )
endif()

# HID app command handler (Report ID 3 — WebHID protocol)
if(CONFIG_AKIRA_HID_APP_HANDLER)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/hid/hid_app_handler.c)
endif()

# mDNS / DNS-SD device advertisement
if(CONFIG_AKIRA_MDNS)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/net/mdns_service.c)
endif()

# ===== Client Connectivity =====

# WebSocket Client
if(CONFIG_AKIRA_WS_CLIENT)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/client/ws_client.c)
endif()

# CoAP Client
if(CONFIG_AKIRA_COAP_CLIENT)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/client/coap_client.c)
endif()

# ===== Cloud Connectivity (Unified) =====

if(CONFIG_AKIRA_CLOUD_CLIENT)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/connectivity/cloud/cloud_protocol.c
        ${AKIRA_OS_DIR}/src/connectivity/cloud/cloud_client.c
    )

    # App handler (WASM app download/update)
    if(CONFIG_AKIRA_CLOUD_APP_HANDLER)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/cloud/cloud_app_handler.c)
    endif()

    # OTA handler (firmware updates)
    if(CONFIG_AKIRA_CLOUD_OTA_HANDLER)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/cloud/cloud_ota_handler.c)
    endif()
endif()

# ===== Advanced Connectivity Layer =====

# WiFi Manager — centralized connect/disconnect/stats
if(CONFIG_AKIRA_WIFI_MANAGER)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/wifi/wifi_manager.c)
endif()

# Radio Abstraction Layer (RAL)
if(CONFIG_AKIRA_RADIO_MANAGER)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/connectivity/radio/radio_manager.c
    )

    # WiFi radio backend
    if(CONFIG_WIFI)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/radio/radio_wifi.c)
    endif()

    # BLE radio backend
    if(CONFIG_BT)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/radio/radio_ble.c)
    endif()

    # 802.15.4 radio backend
    if(CONFIG_NET_L2_IEEE802154)
        zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/radio/radio_802154.c)
    endif()
endif()

# Matter protocol
if(CONFIG_AKIRA_MATTER)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/connectivity/matter/matter_manager.c
    )
    # Note: Full Matter SDK integration requires adding CHIP module
    # include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/zephyr/chip-module/CMakeLists.txt)
endif()

if(CONFIG_AKIRA_MATTER_COPROC_UART)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/runtime/akira_matter_ipc.c)
endif()

if(CONFIG_AKIRA_MATTER_COPROC_MOCK)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/matter/matter_coproc_mock.c)
endif()

if(CONFIG_AKIRA_WASM_MATTER)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_matter_api.c)
endif()

# MQTT / Home Assistant
if(CONFIG_AKIRA_MQTT)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/connectivity/mqtt/mqtt_service.c
        ${AKIRA_OS_DIR}/src/connectivity/mqtt/ha_discovery.c
    )
endif()

if(CONFIG_AKIRA_WASM_MQTT)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_mqtt_api.c)
endif()

# Thread protocol
if(CONFIG_AKIRA_THREAD)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/connectivity/thread/thread_manager.c
    )
    # Note: Full OpenThread integration requires adding OpenThread module
    # The OpenThread CMake integration is handled via west.yml manifest
endif()

# AkiraMesh protocol
if(CONFIG_AKIRA_MESH)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/connectivity/mesh/mesh_manager.c
        ${AKIRA_OS_DIR}/src/connectivity/mesh/mesh_routing.c
        ${AKIRA_OS_DIR}/src/connectivity/mesh/mesh_crypto.c
        ${AKIRA_OS_DIR}/src/connectivity/mesh/mesh_session.c
    )
endif()

if(CONFIG_AKIRA_WASM_MESH)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/api/akira_mesh_api.c)
endif()

# Services (Unified WASM runtime wrapper)
# Using single unified runtime implementation in src/runtime
# Runtime and native API registration happen in src/runtime/akira_runtime.c
# (No external akira_native_api.c is built.)

# Filesystem Manager (MUST be before App Manager)
zephyr_library_sources(${AKIRA_OS_DIR}/src/storage/fs_manager.c)

# SD card hardware init (disk probe + FATFS mount at /SD:)
if(CONFIG_AKIRA_SD_CARD)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/storage/sd_card.c)
endif()

# FUSB302 VBUS sense (USB trust-prompt trigger)
if(CONFIG_AKIRA_FUSB302_VBUS)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/drivers/usbc/fusb302_vbus.c)
endif()

# Storage drivers for app sources
if(CONFIG_AKIRA_APP_SOURCE_SD)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/storage/sd_manager.c)
endif()

if(CONFIG_AKIRA_APP_SOURCE_USB)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/storage/usb_storage.c)
endif()

# BLE app transfer
if(CONFIG_AKIRA_APP_SOURCE_BLE)
    zephyr_library_sources(${AKIRA_OS_DIR}/src/connectivity/bluetooth/bt_app_transfer.c)
endif()

# ===== Native OS Shell (akira_display_* framebuffer renderer) =====
if(CONFIG_AKIRA_OS_SHELL)
    zephyr_library_sources(
        ${AKIRA_OS_DIR}/src/console_shell/akira_os_shell.c
        ${AKIRA_OS_DIR}/src/console_shell/display_ownership.c
        ${AKIRA_OS_DIR}/src/console_shell/home_screen.c
        ${AKIRA_OS_DIR}/src/console_shell/shell_theme.c
        ${AKIRA_OS_DIR}/src/console_shell/sd_install_screen.c
        ${AKIRA_OS_DIR}/src/console_shell/install_progress_screen.c
        ${AKIRA_OS_DIR}/src/console_shell/settings_screen.c
        ${AKIRA_OS_DIR}/src/console_shell/settings/wifi_screen.c
        ${AKIRA_OS_DIR}/src/console_shell/settings/display_screen.c
        ${AKIRA_OS_DIR}/src/console_shell/settings/ble_screen.c
        ${AKIRA_OS_DIR}/src/console_shell/settings/power_screen.c
        ${AKIRA_OS_DIR}/src/console_shell/settings/ota_screen.c
        ${AKIRA_OS_DIR}/src/console_shell/settings/datetime_screen.c
        ${AKIRA_OS_DIR}/src/console_shell/settings/apps_screen.c
        ${AKIRA_OS_DIR}/src/console_shell/settings/devmode_screen.c
    )
    target_include_directories(akira_os INTERFACE ${AKIRA_OS_DIR}/src/console_shell)
endif()

# Include directories
target_include_directories(akira_os INTERFACE
    ${AKIRA_OS_DIR}/include/
    ${AKIRA_OS_DIR}/src/
    ${AKIRA_OS_DIR}/src/akira/
    ${AKIRA_OS_DIR}/src/api/
    ${AKIRA_OS_DIR}/src/apps/
    ${AKIRA_OS_DIR}/src/connectivity/
    ${AKIRA_OS_DIR}/src/connectivity/bluetooth/
    ${AKIRA_OS_DIR}/src/connectivity/client/
    ${AKIRA_OS_DIR}/src/connectivity/cloud/
    ${AKIRA_OS_DIR}/src/connectivity/hid/
    ${AKIRA_OS_DIR}/src/connectivity/http/
    ${AKIRA_OS_DIR}/src/connectivity/usb/
    ${AKIRA_OS_DIR}/src/drivers/
    ${AKIRA_OS_DIR}/src/ipc/
    ${AKIRA_OS_DIR}/src/lib/
    ${AKIRA_OS_DIR}/src/OTA/
    ${AKIRA_OS_DIR}/src/power/
    ${AKIRA_OS_DIR}/src/resource/
    ${AKIRA_OS_DIR}/src/runtime/
    ${AKIRA_OS_DIR}/src/security/
    ${AKIRA_OS_DIR}/src/runtime/security/
    ${AKIRA_OS_DIR}/src/services/
    ${AKIRA_OS_DIR}/src/settings/
    ${AKIRA_OS_DIR}/src/shell/
    ${AKIRA_OS_DIR}/src/storage/
)
