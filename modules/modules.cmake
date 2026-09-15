# SPDX-License-Identifier: Apache-2.0
#
# Zephyr module glue for the akira-os module. Registered by
# `module_ext_root: .` in zephyr/module.yml, so applications do not pass
# -DMODULE_EXT_ROOT. Zephyr includes this file early, before it processes
# snippets, boards' Kconfig and devicetree.
#
# wasm-micro-runtime declares cmake-ext and kconfig-ext in its zephyr/module.yml
# but ships no Zephyr glue. AkiraOS builds WAMR itself (zephyr/CMakeLists.txt),
# so only point its Kconfig at an empty stub for Zephyr's generated `osource`.
set(ZEPHYR_WASM_MICRO_RUNTIME_KCONFIG ${CMAKE_CURRENT_LIST_DIR}/wamr/Kconfig)

# Snippets an application requires, e.g. set(AKIRA_SNIPPETS akira-board) before
# find_package(Zephyr). They are applied before any snippets the user passes with
# `west build -S`, which therefore override them. Does nothing for applications
# that do not set AKIRA_SNIPPETS (MCUboot, unrelated apps).
if(DEFINED AKIRA_SNIPPETS)
  include(${CMAKE_CURRENT_LIST_DIR}/../cmake/akira_snippets.cmake)
  akira_require_snippets(${AKIRA_SNIPPETS})
endif()
