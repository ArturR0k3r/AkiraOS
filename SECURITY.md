# Security Policy

## Reporting a Vulnerability

**Do not open a public GitHub issue for security vulnerabilities.**

Report security issues by email to:

**security@pen.engineering**

Include in your report:

- Description of the vulnerability and its potential impact
- Steps to reproduce or a proof-of-concept (where safe to share)
- Affected versions and components
- Any suggested mitigations if you have them

You will receive an acknowledgement within **48 hours** and a status update within **7 days**.

---

## Supported Versions

| Version | Supported |
|---------|-----------|
| v1.6.x (development, untagged) | Yes — fixes land here first |
| v1.5.x (current release) | Yes — security patches actively backported |
| v1.4.x | Yes — critical patches only |
| v1.3.x and earlier | No — please upgrade |

---

## Disclosure Policy

We follow **coordinated disclosure**:

1. You report to security@pen.engineering.
2. We confirm receipt within 48 hours.
3. We investigate and develop a fix (target: 14 days for critical, 30 days for high severity).
4. We notify you when a fix is ready and agree on a disclosure date.
5. We publish a security advisory and release patched versions.
6. You may publish your findings after the agreed disclosure date.

If we cannot reproduce or confirm the issue within 7 days, we will tell you and explain why.

We will not take legal action against researchers who act in good faith under this policy.

---

## Severity Classification

We use CVSS v3.1 base scores:

| Severity | Score | Example |
|----------|-------|---------|
| Critical | 9.0–10.0 | Remote code execution in the WASM runtime |
| High | 7.0–8.9 | Capability system bypass, sandbox escape |
| Medium | 4.0–6.9 | Privilege escalation within app trust levels |
| Low | 0.1–3.9 | Information disclosure, denial of service to single app |

---

## Security Architecture Summary

AkiraOS implements defence-in-depth across four layers:

1. **WebAssembly sandbox** — WAMR with per-app linear memory; native APIs check every app pointer over its full length
2. **Capability-based access control** — capability bits checked on every native API call; apps request them in their embedded manifest, and apps no signature verifier attested never get privileged ones in a release build
3. **Verified boot** — with a product key (`build.sh -k`), MCUboot verifies the image signature (ECDSA P-256, Ed25519 or RSA) on every boot; the bootloader's own root of trust is the chip's secure boot, which the integrator enables
4. **OTA integrity** — signed images, MCUboot downgrade prevention, trial boot with automatic revert (boot guard)

The configuration these claims apply to is described in [docs/compliance/sesip-evaluated-configuration.md](docs/compliance/sesip-evaluated-configuration.md).

Known limitations are documented in [docs/architecture/security.md](docs/architecture/security.md).

---

## Compliance Documentation

| Document | Status | Description |
|----------|--------|-------------|
| [SBOM (CycloneDX 1.4)](docs/compliance/sbom-cyclonedx.md) | Available | Software Bill of Materials — generate via `./build.sh -s` → `build/sbom.cdx.json` |
| PSA Certified Level 1 Gap Analysis | Planned | Self-certification questionnaire mapping and remaining gaps — not yet written |
| IEC 62443-4-2 SL 1 Mapping | Planned | IACS component security requirement → AkiraOS feature mapping — not yet written |

The SBOM is regenerated on every release build and published as a GitHub release asset (EU Cyber Resilience Act 2027 requirement).

---

## Bug Bounty

There is currently no formal bug bounty programme. We acknowledge all confirmed vulnerability reporters in our security advisories and release notes (unless you prefer to remain anonymous).

---

## Security Contact

**PenEngineering S.R.L**  
Email: security@pen.engineering  
PGP key: available on request
