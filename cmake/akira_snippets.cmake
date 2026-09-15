# SPDX-License-Identifier: Apache-2.0
#
# Snippet helpers for applications built on AkiraOS.
#
# Per-board AkiraOS setup (flash partitions, LittleFS/NVS nodes, board tuning)
# ships as Zephyr snippets in the akira-os module (snippets/). An application
# lists the snippets it always needs; the user can add more with
# `west build -S <snippet>`, `-DSNIPPET=...` or the SNIPPET environment variable.
#
#   set(AKIRA_SNIPPETS akira-board)                 # before find_package(Zephyr)
#   find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
#
# The akira-os module does the rest: modules/modules.cmake calls
# akira_require_snippets() during find_package(Zephyr), before Zephyr reads
# SNIPPET, and zephyr/CMakeLists.txt calls akira_check_snippets(). Applications
# never include this file themselves.
#
# Ordering: required snippets come first, in the order given, followed by the
# user's snippets. Later snippets override earlier ones, so a user or product
# snippet can override any AkiraOS board setting.

# akira_require_snippets(<snippet>...)
#
# Call before find_package(Zephyr). Rewrites the SNIPPET cache variable to the
# required snippets followed by the user's own snippets (duplicates dropped).
# The cache is only written when the value changes, so reconfiguring does not
# report a SNIPPET change.
function(akira_require_snippets)
  if(DEFINED CACHE{SNIPPET})
    set(user_snippets "$CACHE{SNIPPET}")
  elseif(DEFINED ENV{SNIPPET})
    set(user_snippets "$ENV{SNIPPET}")
  else()
    set(user_snippets "")
  endif()
  string(REPLACE " " ";" user_snippets "${user_snippets}")

  set(snippets ${ARGN})
  foreach(snippet IN LISTS user_snippets)
    if(snippet AND NOT snippet IN_LIST snippets)
      list(APPEND snippets ${snippet})
    endif()
  endforeach()

  if(NOT "$CACHE{SNIPPET}" STREQUAL "${snippets}")
    set(SNIPPET "${snippets}" CACHE STRING
        "Snippets to apply: AkiraOS-required snippets first, then user snippets" FORCE)
  endif()
  # A normal variable of the same name would hide the cache entry from Zephyr.
  unset(SNIPPET PARENT_SCOPE)
endfunction()

# akira_check_snippets(<snippet>...)
#
# Call after find_package(Zephyr). Fails configuration if a required snippet was
# not applied, e.g. because a sysbuild image-specific SNIPPET replaced the list.
function(akira_check_snippets)
  foreach(snippet IN LISTS ARGN)
    if(NOT snippet IN_LIST SNIPPET_AS_LIST)
      message(FATAL_ERROR
        "AkiraOS: required snippet '${snippet}' is not applied (applied: '${SNIPPET_AS_LIST}'). "
        "It provides the board's AkiraOS flash layout and configuration. "
        "With sysbuild, pass it per image, e.g. -D<image>_SNIPPET=\"${snippet};...\".")
    endif()
  endforeach()
endfunction()
