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
    ${AKIRA_SRC}/console_shell
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
    ${AKIRA_SRC}/console_shell/catalog_net.c
    ${AKIRA_SRC}/console_shell/catalogue_parser.c

    # Stubs
    stubs/stub_mcuboot.c

    # Test suites
    src/test_manifest_parser.c
    src/test_security.c
    src/test_app_lifecycle.c
    src/test_ota_state.c
    src/test_catalog_net.c
    src/test_catalogue_parser.c
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
