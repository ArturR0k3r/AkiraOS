# SPDX-License-Identifier: Apache-2.0
#
# Minimal product firmware built on AkiraOS as a Zephyr module.
#
# This directory stands in for a product repository: it references no AkiraOS
# source file by path. AkiraOS comes from the akira-os module
# (CONFIG_AKIRA_OS=y in prj.conf), its boards from the module's board_root and
# the per-board AkiraOS setup from the akira-board snippet.

cmake_minimum_required(VERSION 3.20.0)

# AkiraOS board setup (flash layout, LittleFS/NVS nodes, board tuning) for the
# selected board, from the akira-os module's akira-board snippet. Snippets passed
# with `west build -S` are applied after it.
set(AKIRA_SNIPPETS akira-board)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(akira_out_of_tree_product)

target_sources(app PRIVATE src/main.c)
target_link_libraries(app PRIVATE akira_os)
