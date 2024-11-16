message("ocre.cmake")

set(ocre_sources
  # Libraries
  ${OCRE_ROOT_DIR}/src/ocre/sm/sm.c
  ${OCRE_ROOT_DIR}/src/ocre/fs/fs.c
  ${OCRE_ROOT_DIR}/src/ocre/ocre_timers/ocre_timer.c
  ${OCRE_ROOT_DIR}/src/ocre/container_healthcheck/ocre_container_healthcheck.c
  ${OCRE_ROOT_DIR}/src/ocre/ocre_container_runtime/ocre_container_runtime.c
  ${OCRE_ROOT_DIR}/src/ocre/component/component.c

  # Components
  ${OCRE_ROOT_DIR}/src/ocre/components/container_supervisor/cs_main.c
  ${OCRE_ROOT_DIR}/src/ocre/components/container_supervisor/cs_sm.c
  ${OCRE_ROOT_DIR}/src/ocre/components/container_supervisor/cs_sm_impl.c
 
  # Ocre APIs
  ${OCRE_ROOT_DIR}/src/ocre/api/ocre_api.c
) 

zephyr_include_directories(
  ${OCRE_ROOT_DIR}/src/
  ${OCRE_ROOT_DIR}/src/ocre
)

