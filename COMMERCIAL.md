# Commercial Licensing

AkiraOS is open source under the **Apache License 2.0**, which permits free use for personal projects, research, and open source products.

If you are building a **commercial product** — embedded firmware, an OEM device, or a managed IoT service — a commercial license removes the attribution requirements and adds enterprise support, legal indemnification, and access to the Akira Platform.

---

## Licensing Tiers

### Community — Free (Apache 2.0)

For makers, students, open source projects, and startups evaluating the platform.

- Full AkiraOS runtime and all WASM APIs
- AkiraSDK with sample applications
- Community support via GitHub Issues and Discussions
- No restrictions on hardware targets
- No fees

### Startup OEM — €3,000 / year per product line

For small hardware companies shipping up to 1,000 devices.

- Commercial license (no open-source obligations for your firmware)
- Access to **AkiraHub** hosted SaaS — device registry, app catalogue, remote install
- `akira-cli` signing service — sign and distribute `.akpkg` apps
- Email support (48-hour response SLO)
- 1 product line definition

### Professional OEM — €15,000 / year per product line

For established hardware companies with active deployments.

Everything in Startup, plus:

- Unlimited devices under one product line
- App signing portal — publish apps to your own branded store
- Priority support (8-hour response SLO)
- One BSP porting consultation session included
- Compliance documentation pack (security model, SBOM export)
- Quarterly roadmap preview calls

### Enterprise — Custom pricing

For industrial, medical, automotive, and large-scale IoT deployments.

Everything in Professional, plus:

- **Self-hosted AkiraHub** — on-premise or private cloud deployment
- Advanced security module — NVS encryption, HSM/secure element integration
- Full compliance documentation — PSA Certified, IEC 62443, CycloneDX SBOM
- White-label SDK — rename and rebrand the API surface for your developer program
- SLA: 4-hour critical / 24-hour standard response
- Dedicated Slack channel with direct engineering access
- Custom feature and driver development (time and materials)
- Patent license and legal indemnification

---

## One-Time Services

| Service | Price |
|---------|-------|
| BSP porting to new hardware | €5,000 – €20,000 |
| Custom driver development | Project rate |
| PSA Certified preparation consulting | €10,000 – €30,000 |
| On-site integration support (per day) | €1,500 |

---

## Frequently Asked Questions

**Do I need a commercial license to sell a product that runs AkiraOS?**

If you distribute your product with AkiraOS source code or binaries and comply with the Apache 2.0 attribution requirements, no commercial license is required. If you want to remove those obligations, or if you need AkiraHub, enterprise security features, or an SLA, a commercial license is the right path.

**Does Apache 2.0 require me to open-source my application code?**

No. Apache 2.0 is a permissive license. Your WASM applications and your product-specific code remain proprietary. Only modifications to AkiraOS itself must be attributed.

**What is a "product line"?**

A product line is a single firmware configuration targeting a family of hardware variants that share the same AkiraOS build (same `prj.conf` + board overlays). Multiple SKUs of the same product count as one product line.

**Can I evaluate enterprise features before purchasing?**

Yes. Contact us for a 30-day evaluation license for the Akira Platform enterprise modules.

**Is there a volume discount for large device fleets?**

Yes. Enterprise pricing is negotiated based on deployed device count, support requirements, and contract length. Contact us.

---

## Contact

To purchase a license or discuss your requirements:

**Email:** enterprise@pen.engineering  
**Website:** [akiraos.dev/enterprise](https://akiraos.dev/enterprise)  
**Company:** PenEngineering S.R.L

Response time: 1 business day.

---

## Third-Party Components

AkiraOS incorporates the following Apache 2.0 licensed components:

- [Zephyr RTOS](https://github.com/zephyrproject-rtos/zephyr) — Apache 2.0
- [WASM Micro Runtime (WAMR)](https://github.com/bytecodealliance/wasm-micro-runtime) — Apache 2.0
- [MCUboot](https://github.com/mcu-tools/mcuboot) — Apache 2.0

These components retain their own copyright notices as required by their licenses.
