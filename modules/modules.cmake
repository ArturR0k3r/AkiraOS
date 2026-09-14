# SPDX-License-Identifier: Apache-2.0
#
# Zephyr module glue for third-party west projects that AkiraOS depends on.
# Registered by `module_ext_root: .` in zephyr/module.yml, so applications do
# not pass -DMODULE_EXT_ROOT.
#
# wasm-micro-runtime declares cmake-ext and kconfig-ext in its zephyr/module.yml
# but ships no Zephyr glue. AkiraOS builds WAMR itself (zephyr/CMakeLists.txt),
# so only point its Kconfig at an empty stub for Zephyr's generated `osource`.
set(ZEPHYR_WASM_MICRO_RUNTIME_KCONFIG ${CMAKE_CURRENT_LIST_DIR}/wamr/Kconfig)
