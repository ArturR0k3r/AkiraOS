# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Make AkiraOS board and DTS roots available
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
list(APPEND DTS_ROOT   ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(akiraos_tests)

# ── Include paths ──────────────────────────────────────────────────────────────
set(AKIRA_SRC ${CMAKE_CURRENT_LIST_DIR}/../src)
set(AKIRA_INC ${CMAKE_CURRENT_LIST_DIR}/../include)

target_include_directories(app PRIVATE
    ${AKIRA_SRC}
    ${AKIRA_SRC}/runtime
    ${AKIRA_SRC}/runtime/security
    ${AKIRA_SRC}/runtime/app_manager
    ${AKIRA_SRC}/connectivity
    ${AKIRA_SRC}/connectivity/ota
    ${AKIRA_SRC}/connectivity/matter
    ${AKIRA_SRC}/connectivity/mqtt
    ${AKIRA_INC}
    ${CMAKE_CURRENT_LIST_DIR}/../AkiraSDK/include
    ${CMAKE_CURRENT_LIST_DIR}/stubs
)

# ── Source files under test ────────────────────────────────────────────────────
target_sources(app PRIVATE
    # Sources under test
    ${AKIRA_SRC}/runtime/manifest_parser.c
    ${AKIRA_SRC}/runtime/security.c
    ${AKIRA_SRC}/runtime/security/app_signing_v2.c
    ${AKIRA_SRC}/runtime/security/sandbox.c
    ${AKIRA_SRC}/connectivity/ota/ota_manager.c

    # Stubs
    stubs/stub_mcuboot.c

    # Matter accessory path + in-firmware mock co-processor
    ${AKIRA_SRC}/runtime/akira_matter_ipc.c
    ${AKIRA_SRC}/connectivity/matter/matter_coproc_mock.c

    # Home Assistant discovery JSON helpers (transport stubbed in the test)
    ${AKIRA_SRC}/connectivity/mqtt/ha_discovery.c

    # Test suites
    src/test_manifest_parser.c
    src/test_security.c
    src/test_app_lifecycle.c
    src/test_ota_state.c
    src/test_matter_accessory.c
    src/test_mqtt_ha.c
    src/test_display_clip.c
)

# The Matter accessory sources are normally gated by Kconfig symbols whose
# dependency chain (AKIRA_MATTER -> AKIRA_RADIO_MANAGER, SERIAL, ...) is not
# satisfiable on native_sim. Define the two relevant switches directly so the
# accessory path + mock compile against the flat test build (same technique as
# the CONFIG_BOOTLOADER_MCUBOOT define below). The mock replaces the UART, so
# no serial hardware is required.
target_compile_definitions(app PRIVATE
    CONFIG_AKIRA_MATTER_ACCESSORY=1
    CONFIG_AKIRA_MATTER_COPROC_MOCK=1
)

# ota_manager.c guards its flash_area and mcuboot includes behind:
#   #if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
# CONFIG_FLASH_MAP=y is set in prj.conf (flash simulator), but
# CONFIG_BOOTLOADER_MCUBOOT cannot be set via Kconfig on native_sim (it would
# select USE_DT_CODE_PARTITION which has unsatisfied SOC dependencies).
# Setting it as a per-file compile definition bypasses Kconfig entirely so
# ota_manager.c compiles with OTA_FLASH_AVAILABLE=1.  The boot_request_upgrade /
# boot_write_img_confirmed symbols are satisfied by stubs/stub_mcuboot.c.
set_source_files_properties(
    ${AKIRA_SRC}/connectivity/ota/ota_manager.c
    PROPERTIES COMPILE_DEFINITIONS "CONFIG_BOOTLOADER_MCUBOOT=1"
)
