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

# ---------------------------------------------------------------------------
# Auto-append display panel overlay based on board .conf.
#
# Recognised selectors:
#   CONFIG_AKIRA_ILI9341_DRIVER=y
#       → appends boards/<base>/<safe>_ili9341.overlay
#   CONFIG_LS0XX=y  or  CONFIG_AKIRA_SHARP_DISPLAY=y
#       → appends boards/<base>/<safe>_sharp.overlay
# ST7789V: no overlay needed — DTS nodes already active by default.
# ---------------------------------------------------------------------------
if(DEFINED BOARD)
  string(REPLACE "/" "_" _akira_board_safe "${BOARD}")
  string(REPLACE "/" ";" _akira_board_parts "${BOARD}")
  list(GET _akira_board_parts 0 _akira_board_base)

  set(_akira_bconf "${CMAKE_CURRENT_LIST_DIR}/boards/${_akira_board_safe}.conf")
  if(NOT EXISTS "${_akira_bconf}")
    set(_akira_bconf "${CMAKE_CURRENT_LIST_DIR}/boards/${_akira_board_base}.conf")
  endif()

  if(EXISTS "${_akira_bconf}")
    file(READ "${_akira_bconf}" _akira_board_conf_text)
    if(_akira_board_conf_text MATCHES
       "(^|\n)[ \t]*CONFIG_AKIRA_ILI9341_DRIVER[ \t]*=[ \t]*y([ \t\n]|$)")
      set(_akira_ili_overlay
          "${CMAKE_CURRENT_LIST_DIR}/boards/${_akira_board_base}/${_akira_board_safe}_ili9341.overlay")
      if(EXISTS "${_akira_ili_overlay}")
        list(APPEND EXTRA_DTC_OVERLAY_FILE "${_akira_ili_overlay}")
        message(STATUS "AkiraOS: ILI9341 overlay → ${_akira_ili_overlay}")
      else()
        message(WARNING
          "AkiraOS: ILI9341 panel selected but overlay not found: ${_akira_ili_overlay}")
      endif()
    endif()
    if(_akira_board_conf_text MATCHES
       "(^|\n)[ \t]*CONFIG_(LS0XX|AKIRA_SHARP_DISPLAY)[ \t]*=[ \t]*y([ \t\n]|$)")
      set(_akira_sharp_overlay
          "${CMAKE_CURRENT_LIST_DIR}/boards/${_akira_board_base}/${_akira_board_safe}_sharp.overlay")
      if(EXISTS "${_akira_sharp_overlay}")
        list(APPEND EXTRA_DTC_OVERLAY_FILE "${_akira_sharp_overlay}")
        message(STATUS "AkiraOS: Sharp overlay → ${_akira_sharp_overlay}")
      else()
        message(WARNING
          "AkiraOS: Sharp panel selected but overlay not found: ${_akira_sharp_overlay}")
      endif()
    endif()
  endif()
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
