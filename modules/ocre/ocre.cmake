message("ocre.cmake")

set(ocre_component_sources
  # Component support
  ${OCRE_ROOT_DIR}/src/ocre/component/component.c

  # Components
  ${OCRE_ROOT_DIR}/src/ocre/ocre_container_runtime/ocre_container_runtime.c
  ${OCRE_ROOT_DIR}/src/ocre/components/container_supervisor/cs_main.c
  ${OCRE_ROOT_DIR}/src/ocre/components/container_supervisor/cs_sm.c
  ${OCRE_ROOT_DIR}/src/ocre/components/container_supervisor/cs_sm_impl.c
)

# Collect all sources in one list
set(ocre_lib_sources
    # Libraries
    ${OCRE_ROOT_DIR}/src/ocre/sm/sm.c
    ${OCRE_ROOT_DIR}/src/shared/platform/zephyr/core_fs.c
    ${OCRE_ROOT_DIR}/src/shared/platform/zephyr/core_thread.c
    ${OCRE_ROOT_DIR}/src/shared/platform/zephyr/core_mutex.c
    ${OCRE_ROOT_DIR}/src/shared/platform/zephyr/core_mq.c
    ${OCRE_ROOT_DIR}/src/shared/platform/zephyr/core_misc.c
    ${OCRE_ROOT_DIR}/src/shared/platform/zephyr/core_memory.c
    ${OCRE_ROOT_DIR}/src/shared/platform/zephyr/core_timer.c
    ${OCRE_ROOT_DIR}/src/ocre/ocre_timers/ocre_timer.c
    ${OCRE_ROOT_DIR}/src/ocre/shell/ocre_shell.c

    # Ocre APIs
    ${OCRE_ROOT_DIR}/src/ocre/api/ocre_api.c
    ${OCRE_ROOT_DIR}/src/ocre/api/ocre_common.c
)

# Conditionally add sources
if(CONFIG_OCRE_SENSORS)
    list(APPEND ocre_lib_sources ${OCRE_ROOT_DIR}/src/ocre/ocre_sensors/ocre_sensors.c)
endif()

if(CONFIG_RNG_SENSOR)
    list(APPEND ocre_lib_sources ${OCRE_ROOT_DIR}/src/ocre/ocre_sensors/rng_sensor.c)
endif()

if(DEFINED CONFIG_OCRE_GPIO)
    list(APPEND ocre_lib_sources ${OCRE_ROOT_DIR}/src/ocre/ocre_gpio/ocre_gpio.c)
endif()

if(CONFIG_OCRE_CONTAINER_MESSAGING)
    list(APPEND ocre_lib_sources ${OCRE_ROOT_DIR}/src/ocre/ocre_messaging/ocre_messaging.c)
endif()


zephyr_include_directories(
    ${OCRE_ROOT_DIR}/src/
    ${OCRE_ROOT_DIR}/src/ocre
    ${OCRE_ROOT_DIR}/src/shared/platform
)

# OCRE CMake integration file

# Determine the ISA of the target and set appropriately for WAMR
if (DEFINED CONFIG_ISA_THUMB2)
    set(TARGET_ISA THUMB)
elseif (DEFINED CONFIG_ISA_ARM)
    set(TARGET_ISA ARM)
elseif (DEFINED CONFIG_X86)
    set(TARGET_ISA X86_32)
elseif (DEFINED CONFIG_XTENSA)
    set(TARGET_ISA XTENSA)
elseif (DEFINED CONFIG_RISCV)
    set(TARGET_ISA RISCV32)
elseif (DEFINED CONFIG_ARCH_POSIX)
    set(TARGET_ISA X86_32)  
else ()
    message(WARNING "Unsupported ISA: ${CONFIG_ARCH}, defaulting to X86_32")
    set(TARGET_ISA X86_32)
endif ()
message("OCRE WAMR TARGET ISA: ${TARGET_ISA}")

# WAMR Configuration Options
set(WAMR_BUILD_PLATFORM "zephyr")
set(WAMR_BUILD_TARGET ${TARGET_ISA})
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 0)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)

# LIBC configuration - Use builtin for Xtensa/ESP32 and RISC-V/ESP32-C3 (no sys/random.h)
if(TARGET_ISA STREQUAL "XTENSA" OR TARGET_ISA STREQUAL "RISCV32")
    set(WAMR_BUILD_LIBC_BUILTIN 1)
    set(WAMR_BUILD_LIBC_WASI 0)
    set(WAMR_BUILD_LIB_PTHREAD 0)
else()
    set(WAMR_BUILD_LIBC_BUILTIN 0)
    set(WAMR_BUILD_LIBC_WASI 1)
    set(WAMR_BUILD_LIB_PTHREAD 1)
endif()

set(WAMR_BUILD_REF_TYPES 1)
set(WASM_ENABLE_LOG 1)

if(NOT DEFINED WAMR_BUILD_GLOBAL_HEAP_POOL)
    set(WAMR_BUILD_GLOBAL_HEAP_POOL 1)
endif()
if(NOT DEFINED WAMR_BUILD_GLOBAL_HEAP_SIZE)
    set(WAMR_BUILD_GLOBAL_HEAP_SIZE 32767)
endif()

# Set WAMR root directory
set(WAMR_ROOT_DIR "${OCRE_ROOT_DIR}/wasm-micro-runtime")

# Include WAMR's runtime library build script
# This will populate WAMR_RUNTIME_LIB_SOURCE with all necessary WAMR source files
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

# Add WAMR sources to OCRE library
list(APPEND ocre_lib_sources ${WAMR_RUNTIME_LIB_SOURCE})