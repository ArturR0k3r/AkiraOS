---
layout: default
title: SESIP Evaluated Configuration
parent: Resources
nav_order: 5
---

# SESIP evaluated configuration

The AkiraOS Security Target (SESIP Level 2) describes one configuration: AkiraOS
built with the `akira-release` snippet, signed with a product key, and booted by
an MCUboot built with that key. This page is the preparative and operational
guidance for that configuration. A build outside it is a development build and
the Security Target does not cover it.

## Build the evaluated configuration

Generate the product keys once and keep them out of git
([Signing & Release](../development/signing-and-release.md)):

```bash
west akira keygen --out keys --mcuboot-type ecdsa-p256
```

Build the bootloader and the application together:

```bash
./build.sh -b akiraconsole_prod -bl y -k keys/mcuboot-signing.pem --release
```

The build stops with an error unless every check below passes:

- `-k` builds MCUboot with `BOOT_SIGNATURE_TYPE_*` matching the key, the key's
  public half, `MCUBOOT_DOWNGRADE_PREVENTION` and `BOOT_VALIDATE_SLOT0`
  (`boards/mcuboot-secure-update.conf`), and signs the application with the key.
- `--release` checks the built MCUboot `.config`: a signature type other than
  `NONE`, a key that is not one of MCUboot's published sample keys, and
  downgrade prevention on.
- `--release` applies the `akira-release` snippet, which sets
  `CONFIG_AKIRA_RELEASE_BUILD=y`. Configuration then fails on any setting
  listed in `cmake/akira_release_checks.cmake`.

Without `build.sh`: build MCUboot with the same options, then
`west build -b <board> AkiraOS -S akira-release -- -DCONFIG_MCUBOOT_GENERATE_UNSIGNED_IMAGE=n -DCONFIG_MCUBOOT_SIGNATURE_KEY_FILE=\"keys/mcuboot-signing.pem\"`.

## What the configuration enforces

| Setting | Effect | Security Target SFR |
|---|---|---|
| MCUboot signature type + product key | MCUboot boots and installs only images signed with the product key | Secure Update of Platform |
| `MCUBOOT_DOWNGRADE_PREVENTION` | MCUboot erases a staged image older than the running one | Secure Update of Platform |
| `AKIRA_OTA_REFUSE_DOWNGRADE` | The device refuses an older image before rebooting and reports it to the sender | Secure Update of Platform |
| MCUboot swap-using-move + boot guard | A new image runs as a trial and reverts unless it boots healthy | Secure Update of Platform |
| Encrypted GATT permissions on BLE OTA and app transfer | Firmware and apps over BLE need a paired, encrypted link | Secure Update of Platform |
| `AKIRA_HTTP_SERVER=n` | No plain-HTTP firmware upload, app install or app control | Secure Update of Platform |
| WAMR interpreter with full module validation (`AKIRA_WAMR_MINI_LOADER=n`) | Every app's bytecode is validated before it runs | Isolation of Platform |
| Native API range checks | Every pointer an app passes is checked against its own linear memory for the full length | Isolation of Platform, Isolation of Application Parts |
| `AKIRA_HARDEN_UNATTESTED_APPS` | An app no verifier attested gets no privileged capability and cannot load AOT native code | Isolation of Application Parts |
| `akira version`, `GET /api/v1/info` | Reports AkiraOS version and source revision, the MCUboot-verified image version, Zephyr, WAMR and their revisions, board, device ID | Verification of Platform Identity |
| `AKIRA_SHELL_DEBUG_COMMANDS=n`, no telnet shell | No arbitrary memory read, no network shell | Secure Debugging (not claimed: debug is disabled) |

## What the integrator must provide

These are the Security Target's objectives for the operational environment.
The configuration above does not meet them on its own.

1. **Root of trust for the bootloader.** Enable the chip's ROM verification of
   MCUboot and flash encryption before devices ship. On ESP32-S3 these are the
   Secure Boot V2 and flash-encryption eFuses; follow Espressif's documentation
   for the toolchain version in use. Without this, MCUboot itself can be
   replaced by anyone who can write the flash.
2. **Locked debug and download ports.** Disable JTAG and the UART download mode
   (ESP32-S3: `DIS_PAD_JTAG`, `DIS_USB_JTAG`, `DIS_DOWNLOAD_MODE` or secure
   download mode) or the equivalent option bytes on other chips.
3. **No reachable serial shell.** `akira-release` keeps the serial/USB shell,
   which has no login. Either build with `CONFIG_SHELL=n` or make sure the
   console port cannot be reached in the shipped product. Release builds print
   this as an advisory.
4. **Key management.** Generate signing keys off-device, keep the private
   halves in a secret store, and never use MCUboot's sample keys. People who
   can sign firmware or apps must be trusted.
5. **Device identity.** Provision a unique device identity at manufacture if
   the product needs one beyond the hardware ID that `akira version` reports.

## Operational guidance

- **Check a device.** Run `akira version` on the console, or
  `imgtool verify -k keys/mcuboot-signing.pem zephyr.signed.bin` on a release
  image. The `Image` line is the version MCUboot verified.
- **Update.** Ship only images signed with the product key and stamped with a
  higher `VERSION` than the fleet runs. Same-version images are accepted;
  older ones are refused.
- **Apps.** Without a platform signature verifier (`AKIRA_REQUIRE_SIGNED_APPS`
  with AkiraPlatform), every app runs as unattested: interpreted WASM only,
  no privileged capability. Review an app's manifest before installing it.
- **App data.** Each app's files live under `/akira/apps/<app name>/`. The name
  comes from the app's manifest, so without signatures a second app that
  declares the same name reaches the same files. Install apps only from
  sources you trust with each other's data, or use a signature verifier.
- **Flaws.** Report vulnerabilities as described in `SECURITY.md`.

## Not covered

The Security Target does not claim these in this configuration: TLS or any
other secure channel, attestation, encrypted storage bound to a hardware key,
and app signature verification without AkiraPlatform's verifier.
