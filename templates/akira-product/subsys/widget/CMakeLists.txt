# SPDX-License-Identifier: Apache-2.0
#
# The product subsystem. Its AKIRA_CAPABILITY_DEFINE / AKIRA_NATIVE_API_DEFINE /
# AKIRA_HOOK_DEFINE entries live in linker iterable sections that are only kept
# if the object is whole-archived. An application's own zephyr_library() is
# registered after find_package(Zephyr) and misses Zephyr's whole-archive
# capture, so its section entries would be dropped. Compiling into the always-
# linked `app` target keeps them. (A larger product can instead ship the
# subsystem as its own Zephyr module, which is processed during find_package
# like akira-os itself.)
if(CONFIG_WIDGET)
  target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/widget.c)
endif()
