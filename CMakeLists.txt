cmake_minimum_required(VERSION 3.20.0w)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(AkiraOS LANGUAGES C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Determine the ISA of the target and set appropriately
if (DEFINED CONFIG_ISA_THUMB2)
set (TARGET_ISA THUMB)
elseif (DEFINED CONFIG_ISA_ARM)
set (TARGET_ISA ARM)
elseif (DEFINED CONFIG_X86)
set (TARGET_ISA X86_32)
elseif (DEFINED CONFIG_XTENSA)
set (TARGET_ISA XTENSA)
elseif (DEFINED CONFIG_RISCV)
set (TARGET_ISA RISCV32)
elseif (DEFINED CONFIG_ARCH_POSIX)
# Technically, this is not correct as the CPU architecture is not set.  This assumes POSIX is x86 32-bit
set (TARGET_ISA X86_32)  
else ()
message (FATAL_ERROR "Unsupported ISA: ${CONFIG_ARCH}")
endif ()
message("TARGET ISA: ${TARGET_ISA}")

##################
#  WAMR Options  #
##################
set (WAMR_BUILD_PLATFORM "zephyr")
set (WAMR_BUILD_TARGET ${TARGET_ISA})
set (WAMR_BUILD_INTERP 1)
set (WAMR_BUILD_FAST_INTERP 0)
set (WAMR_BUILD_AOT 0)
set (WAMR_BUILD_JIT 0)
set (WAMR_BUILD_LIBC_BUILTIN 1)
set (WAMR_BUILD_LIBC_WASI 0)
set (WAMR_BUILD_APP_FRAMEWORK 0)
set (WAMR_BUILD_APP_LIST WAMR_APP_BUILD_BASE )
set (WAMR_MPU_STACK_COUNT 5)

# Override the global heap usage
if (NOT DEFINED WAMR_BUILD_GLOBAL_HEAP_POOL)
  set (WAMR_BUILD_GLOBAL_HEAP_POOL 1)
endif ()

# Override the global heap size for small devices
if (NOT DEFINED WAMR_BUILD_GLOBAL_HEAP_SIZE)
  #set (WAMR_BUILD_GLOBAL_HEAP_SIZE 131072) # 128 KB
  set (WAMR_BUILD_GLOBAL_HEAP_SIZE 32767) # 32 KB
endif ()

# Include WAMR build script
set (WAMR_ROOT_DIR ${ZEPHYR_WASM_MICRO_RUNTIME_MODULE_DIR})
include (${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

# Include Project Ocre build script
set (OCRE_ROOT_DIR ${ZEPHYR_PROJECT_OCRE_MODULE_DIR})
include (modules/ocre/ocre.cmake)

# Generate the messages header file
set(MSG_INPUT_FILES ${OCRE_ROOT_DIR}/src/ocre/components/container_supervisor/component_supervisor.yaml)
set(MSG_GENERATED_FILE ${CMAKE_CURRENT_LIST_DIR}/src/messaging/messages.g)

add_custom_command(
    OUTPUT ${MSG_GENERATED_FILE}
    COMMAND ${PYTHON_EXECUTABLE} ${OCRE_ROOT_DIR}/tools/automsg ${MSG_INPUT_FILES} ${MSG_GENERATED_FILE}
    DEPENDS ${MSG_INPUT_FILES}
    COMMENT "Running Python script to generate ${MSG_GENERATED_FILE}"
)
add_custom_target(generate_messages DEPENDS ${MSG_GENERATED_FILE})

zephyr_library_include_directories(${ZEPHYR_BASE}/subsys/net/ip)
zephyr_library_link_libraries(LITTLEFS)

set(AkiraOS_sources
  # Libraries
 

  # Generated files


  # Components

) 

# Compile in sensors framework if enabled.
if(DEFINED CONFIG_SENSOR)
#set(lib_sources ${lib_sources} src/ocre/sensors/sensors.c)
endif()

#TODO remove this line afther compleatly moving to zephyr HW v2
get_filename_component(ZEPHYR_PARENT_DIR "${ZEPHYR_BASE}" DIRECTORY)

zephyr_include_directories(
  #TODO remove this line afther compleatly moving to zephyr HW v2
  ${ZEPHYR_PARENT_DIR}/build/zephyr/include/generated/zephyr
  src/
)

target_sources(app PRIVATE ${WAMR_RUNTIME_LIB_SOURCE} ${ocre_sources} ${AkiraOS_sources} src/main.c)
add_dependencies(app generate_messages)
