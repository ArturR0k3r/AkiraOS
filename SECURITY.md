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
| v1.5.x (current) | Yes — security patches actively backported |
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

1. **WebAssembly sandbox** — memory-safe execution, no direct hardware access, bounded linear memory per app
2. **Capability-based access control** — 23 capability bits enforced on every native API call; apps declare requirements in their embedded manifest
3. **Secure boot chain** — MCUboot RSA/ECDSA firmware signature verification; WASM binary SHA-256 integrity check before loading
4. **OTA integrity** — Ed25519-signed firmware bundles (`.akfw`), atomic flash with rollback on failure

Known limitations are documented in [docs/architecture/security.md](docs/architecture/security.md).

---

## Bug Bounty

There is currently no formal bug bounty programme. We acknowledge all confirmed vulnerability reporters in our security advisories and release notes (unless you prefer to remain anonymous).

---

## Security Contact

**PenEngineering S.R.L**  
Email: security@pen.engineering  
PGP key: available on request
