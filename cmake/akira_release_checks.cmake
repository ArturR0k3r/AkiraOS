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
# Without a platform verifier (REQUIRE_SIGNED_APPS) no app is attested, so the
# sandbox alone has to hold: no privileged grants, full WAMR validation.
# Unattested AOT stays the user's choice (CONFIG_AKIRA_REFUSE_UNATTESTED_AOT).
if(CONFIG_AKIRA_WASM_RUNTIME AND NOT CONFIG_AKIRA_REQUIRE_SIGNED_APPS)
  if(NOT CONFIG_AKIRA_HARDEN_UNATTESTED_APPS)
    list(APPEND _akira_findings
      "CONFIG_AKIRA_HARDEN_UNATTESTED_APPS is not set - unattested apps can be granted privileged capabilities")
  endif()
  if(CONFIG_AKIRA_WAMR_MINI_LOADER)
    list(APPEND _akira_findings
      "CONFIG_AKIRA_WAMR_MINI_LOADER=y - WAMR skips module validation for apps no verifier has authenticated")
  endif()
endif()

if(CONFIG_AKIRA_SANDBOX_SELFTEST)
  list(APPEND _akira_findings
    "CONFIG_AKIRA_SANDBOX_SELFTEST=y - apps can call selftest_fault() to crash their own thread on purpose")
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

# --- Debug interfaces -------------------------------------------------------
if(CONFIG_SHELL_BACKEND_TELNET)
  list(APPEND _akira_findings
    "CONFIG_SHELL_BACKEND_TELNET=y - an unauthenticated shell listens on the network")
endif()
if(CONFIG_AKIRA_SHELL_DEBUG_COMMANDS)
  list(APPEND _akira_findings
    "CONFIG_AKIRA_SHELL_DEBUG_COMMANDS=y - `debug memdump` reads arbitrary memory")
endif()
# Zephyr shell modules that give a console (or BLE companion) user raw access.
foreach(_akira_sh DEVMEM_SHELL I2C_SHELL GPIO_SHELL FLASH_SHELL FILE_SYSTEM_SHELL)
  if(CONFIG_${_akira_sh})
    list(APPEND _akira_findings
      "CONFIG_${_akira_sh}=y - the shell (no login) gets raw memory, bus, pin, flash or file access")
  endif()
endforeach()

# --- Bluetooth management ---------------------------------------------------
if(CONFIG_AKIRA_BT_COMPANION_SHELL)
  list(APPEND _akira_findings
    "CONFIG_AKIRA_BT_COMPANION_SHELL=y - the BLE companion runs any shell command")
endif()
if((CONFIG_AKIRA_BT_COMPANION OR CONFIG_AKIRA_APP_SOURCE_BLE) AND
   NOT CONFIG_AKIRA_BT_MGMT_PAIRING_PASSKEY AND NOT CONFIG_AKIRA_BT_MGMT_PAIRING_JUST_WORKS)
  list(APPEND _akira_findings
    "CONFIG_AKIRA_BT_MGMT_PAIRING_PASSKEY or _JUST_WORKS is not set - the BLE companion accepts commands with no pairing at all")
endif()
if(CONFIG_AKIRA_OTA_BLE)
  list(APPEND _akira_findings
    "CONFIG_AKIRA_OTA_BLE=y - any paired BLE peer can erase the update slot (no image hash, no sender check)")
endif()

# --- Settings encryption ----------------------------------------------------
# The default base key is public; mixing in the device ID (readable over the
# download port, BLE address, `akira version`) does not make it secret. Without
# a key provider (AkiraPlatform keystore, eFuse/secure element) every unit's
# encrypted settings - Wi-Fi PSK, broker password, mesh key - are readable
# from a flash dump.
if(CONFIG_AKIRA_SETTINGS_ENCRYPTION AND NOT CONFIG_AKIRA_PLATFORM_SETTINGS_KEY)
  _akira_release_bare_string(CONFIG_AKIRA_SETTINGS_ENCRYPTION_KEY_HEX _akira_skey)
  string(TOLOWER "${_akira_skey}" _akira_skey)
  if(_akira_skey STREQUAL "c3c3a4e60f1991e85d7796890710e99338a4c5ac93bffc5c03dcf662bfcf9dee")
    list(APPEND _akira_findings
      "CONFIG_AKIRA_SETTINGS_ENCRYPTION_KEY_HEX is the published default - set a product key (and enable flash encryption) or use a settings key provider")
  endif()
endif()

# Advisories: printed in release builds, never fatal. They need a deployment
# decision rather than a code change (see docs/compliance/sesip-evaluated-configuration.md).
set(_akira_advisories "")
if(CONFIG_SHELL)
  list(APPEND _akira_advisories
    "CONFIG_SHELL=y - the serial/USB shell has no login, so turn it off or lock the port in production")
endif()
if((CONFIG_AKIRA_BT_COMPANION OR CONFIG_AKIRA_APP_SOURCE_BLE) AND CONFIG_AKIRA_BT_MGMT_PAIRING_JUST_WORKS)
  if(CONFIG_AKIRA_BT_MGMT_PAIRING_WINDOW_S GREATER 0)
    set(_akira_jw_window "new pairings only ${CONFIG_AKIRA_BT_MGMT_PAIRING_WINDOW_S} s after boot")
  else()
    set(_akira_jw_window "no pairing window, so at any time")
  endif()
  list(APPEND _akira_advisories
    "CONFIG_AKIRA_BT_MGMT_PAIRING_JUST_WORKS=y - any device in BLE range can pair (${_akira_jw_window}) and manage apps, files and settings; use _PASSKEY on devices that can show a passkey")
endif()
if(CONFIG_AKIRA_HID_APP_HANDLER)
  list(APPEND _akira_advisories
    "CONFIG_AKIRA_HID_APP_HANDLER=y - any USB host (a PC, a hostile charger) can install, start, stop and delete apps without confirmation")
endif()
if(CONFIG_AKIRA_RELEASE_BUILD AND _akira_advisories)
  list(JOIN _akira_advisories "\n  - " _akira_advisory_text)
  message(WARNING "AkiraOS release build advisories:\n  - ${_akira_advisory_text}")
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
