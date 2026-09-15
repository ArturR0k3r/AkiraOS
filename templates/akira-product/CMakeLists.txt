# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Board definitions for this product. AkiraOS boards remain available through
# the akira-os module; put your own custom board under boards/ here.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR})

# AkiraOS services (a profile) + per-board AkiraOS setup, from the akira-os
# module. Profile first so a board conf can override a profile default; swap the
# profile for your product (minimal / connected / console / sensor-node).
set(AKIRA_SNIPPETS akira-profile-minimal akira-board)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(akira_product)

add_subdirectory(subsys/widget)

target_sources(app PRIVATE app/src/main.c)
target_link_libraries(app PRIVATE akira_os)   # AkiraOS include paths
