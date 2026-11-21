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

> **Version scope.** The evaluated configuration is tag `v1.6.6`. AkiraOS 1.7
> adds a baseline app signature check (`CONFIG_AKIRA_APP_PUBKEY`, see
> [Extension points](../api-reference/extension-points.md#app-admission)); it
> is not part of the `v1.6.6` evaluation. AkiraOS 1.7 also tightens
> `akira-release`: it turns off the Zephyr shell modules that read or write
> memory, buses, pins, flash and files, refuses unauthenticated BLE management,
> and refuses the published settings-encryption key. The table below describes
> the current tree.

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
| `AKIRA_BT_SECURE_PAIRING`, `AKIRA_OTA_BLE=n` | BLE app transfer and the BLE companion service accept writes only over an authenticated LE Secure Connections bond (passkey shown by the device, typed on the phone); Just Works pairing is refused outside HID mode; the BLE OTA service is not built | Secure Update of Platform |
| `AKIRA_HTTP_SERVER=n` | No plain-HTTP firmware upload, app install or app control | Secure Update of Platform |
| WAMR interpreter with full module validation (`AKIRA_WAMR_MINI_LOADER=n`) | Every app's bytecode is validated before it runs | Isolation of Platform |
| Native API range checks | Every pointer an app passes is checked against its own linear memory for the full length | Isolation of Platform, Isolation of Application Parts |
| `AKIRA_HARDEN_UNATTESTED_APPS` | An app no verifier attested gets no privileged capability | Isolation of Application Parts |
| `akira version`, `GET /api/v1/info` | Reports AkiraOS version and source revision, the MCUboot-verified image version, Zephyr, WAMR and their revisions, board, device ID | Verification of Platform Identity |
| `AKIRA_SHELL_DEBUG_COMMANDS=n`, `DEVMEM_SHELL=n`, `I2C_SHELL=n`, `GPIO_SHELL=n`, `FLASH_SHELL=n`, `FILE_SYSTEM_SHELL=n`, `AKIRA_BT_COMPANION_SHELL=n`, no telnet shell | No arbitrary memory read or write, no raw bus, pin, flash or file access from the shell, no shell over BLE or the network | Secure Debugging (not claimed: debug is disabled) |
| Encrypted settings never exported | `settings get`/`list` and the BLE companion show `[ENCRYPTED]` instead of a decrypted value (Wi-Fi PSK, broker password, keys) | (supports integrator objective 6) |

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
   which has no login (the memory, bus, pin, flash and file commands are off,
   but app and device management commands remain). Either build with
   `CONFIG_SHELL=n` or make sure the console port cannot be reached in the
   shipped product. Release builds print this as an advisory.
4. **Key management.** Generate signing keys off-device, keep the private
   halves in a secret store, and never use MCUboot's sample keys. People who
   can sign firmware or apps must be trusted.
5. **Device identity.** Provision a unique device identity at manufacture if
   the product needs one beyond the hardware ID that `akira version` reports.
6. **A secret settings key.** Encrypted settings are only as secret as their
   key. The release checks refuse the published default
   `AKIRA_SETTINGS_ENCRYPTION_KEY_HEX`; a product key compiled into firmware is
   still readable from a flash dump unless flash encryption is on. Prefer a key
   provider backed by eFuse or a secure element (AkiraPlatform's keystore).
7. **Showing the pairing passkey.** BLE management pairs by passkey entry: the
   product must show `AKIRA_HOOK_BT_PASSKEY_DISPLAY`'s passkey on its display.
   Without that, pairing needs the serial console.

## Operational guidance

- **Check a device.** Run `akira version` on the console, or
  `imgtool verify -k keys/mcuboot-signing.pem zephyr.signed.bin` on a release
  image. The `Image` line is the version MCUboot verified.
- **Update.** Ship only images signed with the product key and stamped with a
  higher `VERSION` than the fleet runs. Same-version images are accepted;
  older ones are refused.
- **Apps.** Without a platform signature verifier (`AKIRA_REQUIRE_SIGNED_APPS`
  with AkiraPlatform), every app runs as unattested, with no privileged
  capability. Review an app's manifest before installing it.
- **AOT apps.** An `.aot` file is native code. The isolation above holds for
  it only if wamrc built it from a source you trust; a hand-crafted one can
  take over the device. Unattested AOT apps load (with a warning and an audit
  record), so installing one is the user's risk. Build with
  `CONFIG_AKIRA_REFUSE_UNATTESTED_AOT=y` to run AOT only for attested apps.
- **App data.** Each app's files live under `<mount>/apps/<app name>/`
  (`/SD:` with a card, `/lfs` without). App names are lowercase
  `[a-z0-9_.-]`, so FAT's case folding can't alias two apps. An update of an
  attested app must come from the same signer; uninstalling an app deletes its
  data folder and settings. Without signatures, though, the name comes from
  the app's manifest, so a later unsigned app that declares the same name as
  an unsigned app installed earlier replaces it and reaches its files. Install
  apps only from sources you trust with each other's data, or use a signature
  verifier.
- **Flaws.** Report vulnerabilities as described in `SECURITY.md`.

## Not covered

The Security Target does not claim these in this configuration: TLS or any
other secure channel, attestation, encrypted storage bound to a hardware key,
app signature verification without AkiraPlatform's verifier, and isolation of
unattested AOT apps (see Operational guidance).
