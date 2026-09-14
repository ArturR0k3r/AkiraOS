# SPDX-License-Identifier: Apache-2.0
#
# Minimal product firmware built on AkiraOS as a Zephyr module.
#
# This directory stands in for a product repository: it references no AkiraOS
# source file by path. AkiraOS comes from the akira-os module
# (CONFIG_AKIRA_OS=y in prj.conf) and its boards from the module's board_root.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(akira_out_of_tree_product)

target_sources(app PRIVATE src/main.c)
target_link_libraries(app PRIVATE akira_os)
