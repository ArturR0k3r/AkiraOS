---
layout: default
title: Signing & Release
parent: Development
nav_order: 8
---

# Signing and releasing a product

A shippable AkiraOS product needs signed firmware, signed apps, and a
reproducible release. The `west akira` command group (provided by the akira-os
module) handles the firmware side and orchestration; `akira-cli` (from AkiraSDK)
handles app packaging and signing.

{: .warning }
`CONFIG_AKIRA_RELEASE_BUILD=y` fails the build if a development-only setting is
still active (unauthenticated HTTP upload, the `tok3n`/empty token, the direct
upload endpoint, unsigned apps, or an image signed with MCUboot's public
development key). Turn it on for release builds.

## 1. Generate product keys — once, kept out of git

```bash
west akira keygen --out keys --mcuboot-type ecdsa-p256
```

This writes two secrets and a `.gitignore` that excludes them:

| File | Signs | Firmware setting |
|---|---|---|
| `keys/mcuboot-signing.pem` | the firmware image (MCUboot verifies it at boot) | `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` |
| `keys/app-signing-ed25519.pem` | WASM apps (`akira-cli sign`) | `CONFIG_AKIRA_APP_PUBKEY` (public half) |

- **Never commit these.** Store them in a secret manager and, for CI, as
  encrypted secrets (see §4).
- `--mcuboot-type` must match the bootloader's configured algorithm.
- **Rotation:** generate a new key, ship it in a signed OTA built with the *old*
  key so devices accept the update, then switch signing to the new key. Keep the
  old public key trusted until every device has migrated.

## 2. Sign the firmware image

```bash
west build -b <board> . -- -DCONFIG_AKIRA_RELEASE_BUILD=y
west akira sign -d build -k keys/mcuboot-signing.pem
# -> build/zephyr/zephyr.signed.bin
```

`west akira sign` wraps `west sign -t imgtool` with your product key.

## 3. Sign and package apps

App signing and `.akpkg` packaging belong to `akira-cli` (AkiraSDK):

```bash
akira-cli sign  app.wasm  --key keys/app-signing-ed25519.pem
akira-cli pack  app.wasm manifest.json -o app.akpkg
```

`west akira pack …` forwards to `akira-cli pack` when it is on `PATH`. Apps must
embed their manifest (not ship it as a sidecar) for capabilities to be attested
on a hardened board — see [Manifest Format](../api-reference/manifest-format.md).

## 4. Cut a release

Locally:

```bash
west akira release -b <board> -s . -k keys/mcuboot-signing.pem -o dist
# dist/: zephyr.signed.bin, zephyr.elf, sbom.cdx.json (CycloneDX 1.4), SHA256SUMS
```

In CI, the module ships a tag-triggered workflow (`.github/workflows/release.yml`,
copied into a product from the template): it builds each release board, signs
with the `MCUBOOT_KEY_PEM` secret, writes the SBOM and `SHA256SUMS`, and uploads
them to the GitHub release. Set the repository secret:

```
MCUBOOT_KEY_PEM = <contents of keys/mcuboot-signing.pem>
```

A build with no key produces an **unsigned** image and warns — development only.

## 5. SBOM

`west akira sbom -d build` writes a CycloneDX 1.4 SBOM (`sbom.cdx.json`) listing
every west project and its pinned revision — Zephyr, WAMR, TFLite Micro, AkiraOS
and your product. `west akira release` produces it automatically. This is the
artifact an EU CRA / supply-chain review asks for.

## Related

- [OTA Updates](ota-updates.md) — delivering the signed image to devices
- [AkiraOS as a west module](../hardware/west-module.md) — product repo setup
- [API stability policy](../api-stability-policy.md) — the WASM ABI contract
