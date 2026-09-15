# SPDX-License-Identifier: Apache-2.0
#
# Release-safety checks for the AkiraOS application image.
#
# Collects development-only security settings that must not ship. A normal
# (development) build reports them as one warning; with
# CONFIG_AKIRA_RELEASE_BUILD=y the same findings fail the configure step.
# Included from the top-level CMakeLists.txt after Kconfig has been imported.

# Kconfig string values may arrive wrapped in double quotes; compare the bare value.
function(_akira_release_bare_string var out)
  set(val "${${var}}")
  string(REGEX REPLACE "^\"(.*)\"$" "\\1" val "${val}")
  set(${out} "${val}" PARENT_SCOPE)
endfunction()

set(_akira_findings "")

# --- HTTP management endpoints ----------------------------------------------
if(CONFIG_AKIRA_HTTP_SERVER)
  if(CONFIG_AKIRA_HTTP_NO_AUTH)
    list(APPEND _akira_findings
      "CONFIG_AKIRA_HTTP_NO_AUTH=y - HTTP endpoints accept requests without a Bearer token")
  else()
    _akira_release_bare_string(CONFIG_AKIRA_HTTP_UPLOAD_TOKEN _akira_token)
    if(_akira_token STREQUAL "")
      list(APPEND _akira_findings
        "CONFIG_AKIRA_HTTP_UPLOAD_TOKEN is empty - upload endpoints are unauthenticated")
    elseif(_akira_token MATCHES "^(tok3n|changeme)$")
      list(APPEND _akira_findings
        "CONFIG_AKIRA_HTTP_UPLOAD_TOKEN is the published development value '${_akira_token}'")
    endif()
  endif()
  if(CONFIG_AKIRA_HTTP_DEV_UPLOAD)
    list(APPEND _akira_findings
      "CONFIG_AKIRA_HTTP_DEV_UPLOAD=y - the direct WASM upload endpoint is registered")
  endif()
endif()

# --- WASM app signing -------------------------------------------------------
if(CONFIG_AKIRA_ALLOW_UNSIGNED_APPS)
  list(APPEND _akira_findings
    "CONFIG_AKIRA_ALLOW_UNSIGNED_APPS=y - unsigned WASM apps are loaded")
endif()
if(NOT CONFIG_AKIRA_APP_SIGNING)
  list(APPEND _akira_findings
    "CONFIG_AKIRA_APP_SIGNING is not set - WASM app signatures are not verified")
endif()

# --- MCUboot firmware images ------------------------------------------------
if(CONFIG_BOOTLOADER_MCUBOOT)
  _akira_release_bare_string(CONFIG_MCUBOOT_SIGNATURE_KEY_FILE _akira_key)
  get_filename_component(_akira_key_name "${_akira_key}" NAME)
  if(_akira_key_name MATCHES "^root-(rsa-2048|rsa-3072|ec-p256|ec-p384|ed25519)(-pkcs8)?\\.pem$")
    list(APPEND _akira_findings
      "CONFIG_MCUBOOT_SIGNATURE_KEY_FILE is MCUboot's public development key (${_akira_key_name}) - anyone can sign images this bootloader accepts")
  endif()
  if(CONFIG_MCUBOOT_GENERATE_UNSIGNED_IMAGE AND _akira_key STREQUAL "")
    list(APPEND _akira_findings
      "CONFIG_MCUBOOT_GENERATE_UNSIGNED_IMAGE=y with no signature key - firmware images are unsigned")
  endif()
endif()

# --- Report -----------------------------------------------------------------
list(LENGTH _akira_findings _akira_finding_count)
if(_akira_finding_count GREATER 0)
  list(JOIN _akira_findings "\n  - " _akira_finding_text)
  if(CONFIG_AKIRA_RELEASE_BUILD)
    message(FATAL_ERROR
      "AkiraOS release build blocked by ${_akira_finding_count} development-only setting(s):\n"
      "  - ${_akira_finding_text}\n"
      "Fix these in the board .conf, or unset CONFIG_AKIRA_RELEASE_BUILD for a development build.")
  else()
    message(WARNING
      "AkiraOS development build: ${_akira_finding_count} setting(s) must not ship:\n"
      "  - ${_akira_finding_text}\n"
      "Set CONFIG_AKIRA_RELEASE_BUILD=y to make these errors.")
  endif()
elseif(CONFIG_AKIRA_RELEASE_BUILD)
  message(STATUS "AkiraOS release checks passed")
endif()
