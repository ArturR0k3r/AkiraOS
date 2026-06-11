# SBOM — CycloneDX 1.4

**Standard:** CycloneDX v1.4
**Format:** CycloneDX JSON
**Product:** AkiraOS v1.5
**Requirement:** EU Cyber Resilience Act 2027

---

## Generating the SBOM

```bash
# Build first, then generate:
./build.sh -b akiraconsole -s

# Output: build-akiraconsole/sbom.cdx.json
# Standalone (no prior build required for native_sim):
./build.sh -s
# Output: build-native_sim/sbom.cdx.json
```

The `-s` flag produces a CycloneDX 1.4 JSON file at `<build_dir>/sbom.cdx.json`.

---

## Component Inventory

| Component | Version | License | Source |
|-----------|---------|---------|--------|
| Zephyr RTOS | 4.3.0 | Apache-2.0 | github.com/zephyrproject-rtos/zephyr |
| WASM Micro Runtime (WAMR) | 2.3.0 | Apache-2.0 | github.com/bytecodealliance/wasm-micro-runtime |
| mbedTLS | 3.5.2 | Apache-2.0 | github.com/Mbed-TLS/mbedtls |
| MCUboot | 2.1.0 | Apache-2.0 | github.com/mcu-tools/mcuboot |
| picolibc | bundled | BSD-3-Clause | Zephyr bundled |
| AkiraOS | 1.5.x | Apache-2.0 | github.com/penengineering/AkiraOS |
| AkiraPlatform | 1.0.x | LicenseRef-Commercial | (private) |

---

## Vulnerability Scanning

```bash
# Grype (Anchore) — fastest
grype sbom:build/sbom.cdx.json --output table

# OSV-Scanner (Google)
osv-scanner --sbom build/sbom.cdx.json

# OWASP Dependency-Check
dependency-check --format JSON --scan build/ --out dep-check-report/
```

---

## Release Distribution

- `sbom.cdx.json` is attached to every GitHub release as `akiraos-<version>-sbom.cdx.json`
- OEM Enterprise customers receive a cosign-signed SBOM:
  `cosign sign-blob --key cosign.key build/sbom.cdx.json`

---

## Keeping the SBOM Current

| Trigger | Action |
|---------|--------|
| Dependency bump in `west.yml` | Re-run `./build.sh -s` in CI |
| New `EXTRA_ZEPHYR_MODULES` added | Add component to `generate_sbom()` in `build.sh` |
| CVE published against a component | Run `grype sbom:build/sbom.cdx.json`; assess severity |
| Firmware release | Sign and publish as release asset |
