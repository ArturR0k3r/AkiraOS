message("ocre.cmake - OCRE Module Integration")

# Get AkiraOS source directory (two levels up from modules/ocre)
get_filename_component(AKIRAOS_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# The new OCRE (OCRE-NEW) has a simplified structure that's designed to be integrated
# as a Zephyr module. We leverage the module system to build OCRE libraries.
# The OCRE module will handle its own CMake configuration through its zephyr/CMakeLists.txt

# CRITICAL: Configure WAMR libc BEFORE including wamr.cmake
# For embedded systems like ESP32 (XTENSA), we must use builtin libc because
# sys/random.h is not available. For host systems, WASI libc is preferred.
# This must be set BEFORE including wamr.cmake to override its defaults.

if(DEFINED CONFIG_XTENSA OR DEFINED CONFIG_RISCV)
    # Embedded systems without POSIX headers (ESP32, ESP32-C3, etc)
    set(WAMR_BUILD_LIBC_BUILTIN 1)
    set(WAMR_BUILD_LIBC_WASI 0)
    set(WAMR_BUILD_LIB_PTHREAD 0)
    message("OCRE: Using WAMR builtin libc for embedded target")
elseif(DEFINED CONFIG_ARCH_POSIX)
    # Host/simulator builds can use WASI libc
    set(WAMR_BUILD_LIBC_BUILTIN 0)
    set(WAMR_BUILD_LIBC_WASI 1)
    set(WAMR_BUILD_LIB_PTHREAD 1)
    message("OCRE: Using WAMR WASI libc for POSIX target")
endif()

# Include OCRE's WAMR configuration (will respect our LIBC settings above)
include(${OCRE_ROOT_DIR}/zephyr/wamr.cmake)

# OCRE is now structured as modular CMake libraries:
# - OcreCore: Core OCRE functionality (container, context, ocre.c)
# - OcrePlatform: Platform-specific code (Zephyr platform layer)
# - OcreRuntime: Runtime container management
# - OcreRuntimeWamr: WAMR runtime engine
# - OcreShell: Shell interface (optional)

# The actual build happens through OCRE's CMakeLists.txt in zephyr/ and src/
# This file is kept for compatibility and future customization if needed