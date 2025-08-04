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

