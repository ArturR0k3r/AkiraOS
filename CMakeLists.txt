cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(AtymRuntime VERSION ${APP_VERSION_MAJOR}.${APP_VERSION_MINOR}.${APP_PATCHLEVEL}.${APP_VERSION_TWEAK} LANGUAGES C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Get the Git commit information and build version string
execute_process(
  	COMMAND git describe --long --dirty --always
  	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  	OUTPUT_VARIABLE RAW_GIT_VERSION
  	OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  string(REPLACE "-dirty" "+" BUILD_INFO ${RAW_GIT_VERSION})

  string(TIMESTAMP BUILD_DATE "%d-%m-%Y, %H:%M" UTC )

  cmake_host_system_information(RESULT BUILD_MACHINE QUERY HOSTNAME)

 	string(PREPEND BUILD_INFO dev-)

  string(REPLACE "-" ";" GIT_INFO_LIST ${RAW_GIT_VERSION})
  list(GET GIT_INFO_LIST 0 VERSION_NUMBER)
  message("VERSION NUMBER: ${VERSION_NUMBER}")
  zephyr_compile_options(-DVERSION_INFO="${VERSION_NUMBER}")
  message("BUILD DATE: ${BUILD_DATE}")
  zephyr_compile_options(-DVERSION_BUILD_DATE="${BUILD_DATE}")
  message("BUILD MACHINE: ${BUILD_MACHINE}")
  zephyr_compile_options(-DVERSION_BUILD_MACHINE="${BUILD_MACHINE}")
  message("BUILD_INFO: ${BUILD_INFO}")
  zephyr_compile_options(-DVERSION_BUILD_INFO="${BUILD_INFO}")

##############

message("Generating ZCBOR serializtion code.")
add_custom_command(
    OUTPUT
      ${CMAKE_CURRENT_LIST_DIR}/src/net/messages/autogen/device_decode.c
      ${CMAKE_CURRENT_LIST_DIR}/src/net/messages/autogen/device_decode.h
      ${CMAKE_CURRENT_LIST_DIR}/src/net/messages/autogen/device_encode.c
      ${CMAKE_CURRENT_LIST_DIR}/src/net/messages/autogen/device_encode.h
      ${CMAKE_CURRENT_LIST_DIR}/src/net/messages/autogen/device_types.h
    COMMAND zcbor code # Invoke code generation
      --decode
      --encode # Generate both encoding and decoding code
      --short-names # Attempt to make generated symbol names shorter (at the risk of collision)
      -c ${CMAKE_CURRENT_LIST_DIR}/src/net/messages/device_api.cddl  # Generate code for the data structures
      -t PublishDeviceStatusRequest
        PublishDeviceStatusResponse
        DescribeDevicePackageResponse
        ExampleDataType
      --output-c ${CMAKE_CURRENT_LIST_DIR}/src/net/messages/autogen/device.c
      --output-h ${CMAKE_CURRENT_LIST_DIR}/src/net/messages/autogen/device.h
      --output-cmake ${CMAKE_CURRENT_LIST_DIR}/src/net/messages/autogen/device.cmake # The generated cmake file will be placed here
    DEPENDS
      ${CMAKE_CURRENT_LIST_DIR}/src/net/messages/device_api.cddl
  )

  ## TODO:  the above needs to be updated.  Here's the proper command:
  ## zcbor code --decode --encode --short-names -c  ./src/ocre/net/messages/device_api.cddl -t publish-device-status-request publish-device-status-response describe-firmware-response list-device-package-apps-response  --output-c ./src/ocre/net/messages/autogen/device.c --output-h ./src/ocre/net/messages/autogen/device.h --output-cmake ./src/ocre/net/messages/autogen/device.cmake 


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
set(MSG_INPUT_FILES ${OCRE_ROOT_DIR}/src/ocre/components/container_supervisor/component_supervisor.yaml ${CMAKE_CURRENT_LIST_DIR}/src/atym/components/device_manager/device_manager.yaml )
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

set(atym_sources
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

target_sources(app PRIVATE ${WAMR_RUNTIME_LIB_SOURCE} ${ocre_sources} ${atym_sources} src/main.c)
add_dependencies(app generate_messages)
