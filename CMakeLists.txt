# SPDX-License-Identifier: Apache-2.0
#
# AkiraOS reference firmware.
#
# AkiraOS itself is the akira-os Zephyr module (zephyr/module.yml): its
# libraries are built by zephyr/CMakeLists.txt and its options live in
# zephyr/Kconfig. This application only provides main() and the board
# configuration, which is the same shape product firmware uses from its own
# repository.

cmake_minimum_required(VERSION 3.20.0)

# Set custom board root - MUST be before find_package(Zephyr)
# BOARD_ROOT should point to directory that CONTAINS a 'boards' subdirectory
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})

# Set custom device tree bindings root - MUST be before find_package(Zephyr)
# DTS_ROOT should point to directory that CONTAINS a 'dts/bindings' subdirectory
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR})

# Register this repository as a Zephyr module even when building outside a
# west workspace. Zephyr de-duplicates modules by name, so this is a no-op
# inside the workspace, where west already provides akira-os.
list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_LIST_DIR})

# Per-board AkiraOS setup (flash layout, LittleFS/NVS nodes, board tuning) and
# the reference display panel come from snippets in the akira-os module
# (snippets/). They are applied before snippets passed with `west build -S`,
# which therefore override them. Products set AKIRA_SNIPPETS the same way.
set(AKIRA_SNIPPETS akira-board)
set(_akira_board "${BOARD}")
if(NOT _akira_board)
  set(_akira_board "$ENV{BOARD}")
endif()
if(_akira_board MATCHES "^akiraconsole_prod(/|$)")
  list(APPEND AKIRA_SNIPPETS akira-display-sharp)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
# The version is owned by the top-level VERSION file, which Zephyr parses into
# APP_VERSION_* during find_package(Zephyr).
if(NOT "${APP_VERSION_MAJOR}" STREQUAL "")
  project(akira_os VERSION ${APP_VERSION_MAJOR}.${APP_VERSION_MINOR}.${APP_PATCHLEVEL})
else()
  project(akira_os)
endif()

# main() calls akira_start(), the boot sequence in the akira-os module.
target_sources(app PRIVATE src/main.c)
target_link_libraries(app PRIVATE akira_os)
