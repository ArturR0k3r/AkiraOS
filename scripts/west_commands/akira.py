# SPDX-License-Identifier: Apache-2.0
"""`west akira` — release helpers for firmware built on AkiraOS.

Subcommands:
  keygen   generate a product MCUboot signing key and an app-signing Ed25519 key
  sbom     write a CycloneDX SBOM of the west workspace for a build
  sign     sign a built MCUboot image with the product key (wraps `west sign`)
  pack     package a WASM app into a .akpkg (delegates to akira-cli)
  release  build + sign + sbom + checksums into a dist/ directory

App packaging/signing is owned by AkiraSDK's `akira-cli`; this command owns the
firmware side and orchestration. Keys are never written into the repo — keep
them out of git and load them from CI secrets.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from west.commands import WestCommand
from west import log


class Akira(WestCommand):
    def __init__(self):
        super().__init__(
            "akira",
            "AkiraOS product release helpers",
            "Generate keys, SBOMs and signed release artifacts for AkiraOS products.",
            accepts_unknown_args=False,
        )

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(self.name, help=self.help,
                                         description=self.description)
        sub = parser.add_subparsers(dest="subcmd", metavar="<subcommand>")
        sub.required = True

        p = sub.add_parser("keygen", help="generate product signing keys")
        p.add_argument("-o", "--out", default="keys", help="output directory (default: keys/)")
        p.add_argument("--mcuboot-type", default="rsa-2048",
                       choices=["rsa-2048", "rsa-3072", "ecdsa-p256", "ed25519"],
                       help="MCUboot key type (must match the bootloader)")
        p.add_argument("-f", "--force", action="store_true", help="overwrite existing keys")

        p = sub.add_parser("sbom", help="write a CycloneDX SBOM for a build")
        p.add_argument("-d", "--build-dir", default="build", help="build directory")
        p.add_argument("-o", "--out", help="output file (default: <build-dir>/sbom.cdx.json)")

        p = sub.add_parser("sign", help="sign a built MCUboot image with the product key")
        p.add_argument("-d", "--build-dir", default="build", help="build directory")
        p.add_argument("-k", "--key", required=True, help="MCUboot signing key (PEM)")

        p = sub.add_parser("pack", help="package a WASM app into a .akpkg (via akira-cli)")
        p.add_argument("args", nargs="*", help="arguments forwarded to `akira-cli pack`")

        p = sub.add_parser("release", help="build + sign + sbom + checksums into dist/")
        p.add_argument("-b", "--board", required=True, help="board to build")
        p.add_argument("-s", "--source-dir", default=".", help="application source dir")
        p.add_argument("-k", "--key", help="MCUboot signing key (PEM); unsigned if omitted")
        p.add_argument("-o", "--out", default="dist", help="output directory (default: dist/)")

        return parser

    def do_run(self, args, unknown):
        getattr(self, f"_{args.subcmd}")(args)

    # ---- keygen ----------------------------------------------------------
    def _keygen(self, args):
        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        mcuboot = out / "mcuboot-signing.pem"
        app = out / "app-signing-ed25519.pem"
        if (mcuboot.exists() or app.exists()) and not args.force:
            log.die(f"keys already exist in {out} (use --force to overwrite)")

        imgtool = shutil.which("imgtool")
        if imgtool:
            self.check_call([imgtool, "keygen", "-k", str(mcuboot), "-t", args.mcuboot_type])
        else:
            log.wrn("imgtool not found; generating the MCUboot key with openssl")
            self._openssl_key(args.mcuboot_type, mcuboot)
        self._openssl_key("ed25519", app)

        gi = out / ".gitignore"
        gi.write_text("# Signing keys — never commit these.\n*.pem\n")
        log.inf(f"Wrote {mcuboot} and {app}")
        log.inf("These are SECRET. Keep them out of git (a .gitignore was added) "
                "and load them from CI secrets. Point CONFIG_MCUBOOT_SIGNATURE_KEY_FILE "
                "at the MCUboot key and CONFIG_AKIRA_APP_PUBKEY at the app public key.")

    def _openssl_key(self, ktype, path):
        openssl = shutil.which("openssl") or log.die("neither imgtool nor openssl found")
        algo = {"rsa-2048": ["genrsa", "2048"], "rsa-3072": ["genrsa", "3072"]}.get(ktype)
        if algo:
            with open(path, "wb") as f:
                subprocess.run([openssl, *algo], check=True, stdout=f)
        elif ktype == "ecdsa-p256":
            self.check_call([openssl, "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(path)])
        else:  # ed25519
            self.check_call([openssl, "genpkey", "-algorithm", "ed25519", "-out", str(path)])

    # ---- sbom ------------------------------------------------------------
    def _sbom(self, args):
        bd = Path(args.build_dir)
        out = Path(args.out) if args.out else bd / "sbom.cdx.json"
        components = []
        try:
            listed = subprocess.run(
                ["west", "list", "-f", "{name}\t{revision}\t{url}"],
                capture_output=True, text=True, check=True).stdout
            for line in listed.splitlines():
                name, rev, url = (line.split("\t") + ["", "", ""])[:3]
                components.append({
                    "type": "library", "name": name, "version": rev or "unknown",
                    "purl": f"pkg:generic/{name}@{rev}" if rev else f"pkg:generic/{name}",
                    "externalReferences": [{"type": "vcs", "url": url}] if url else [],
                })
        except Exception as e:
            log.wrn(f"could not enumerate west projects: {e}")
        bom = {
            "bomFormat": "CycloneDX", "specVersion": "1.4", "version": 1,
            "metadata": {"tools": [{"vendor": "AkiraOS", "name": "west akira sbom"}],
                         "component": {"type": "firmware", "name": "akira-product"}},
            "components": components,
        }
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(bom, indent=2))
        log.inf(f"Wrote {out} ({len(components)} components, CycloneDX 1.4)")

    # ---- sign ------------------------------------------------------------
    def _sign(self, args):
        if not Path(args.key).is_file():
            log.die(f"signing key not found: {args.key}")
        self.check_call(["west", "sign", "-d", args.build_dir, "-t", "imgtool",
                         "--", "--key", args.key])
        log.inf("Signed image: <build-dir>/zephyr/zephyr.signed.bin")

    # ---- pack ------------------------------------------------------------
    def _pack(self, args):
        cli = shutil.which("akira-cli")
        if not cli:
            log.die("akira-cli not found — build it from AkiraSDK/tools/akira-cli "
                    "(it owns .akpkg packaging and app signing)")
        self.check_call([cli, "pack", *args.args])

    # ---- release ---------------------------------------------------------
    def _release(self, args):
        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        bd = out / "build"
        log.inf(f"Building {args.source_dir} for {args.board}")
        self.check_call(["west", "build", "-b", args.board, args.source_dir,
                         "-d", str(bd), "--pristine"])
        if args.key:
            self.check_call(["west", "sign", "-d", str(bd), "-t", "imgtool",
                             "--", "--key", args.key])
            img = bd / "zephyr" / "zephyr.signed.bin"
        else:
            log.wrn("no --key: shipping an UNSIGNED image (development only)")
            img = bd / "zephyr" / "zephyr.bin"
        self._sbom(type("A", (), {"build_dir": str(bd), "out": str(out / "sbom.cdx.json")}))
        artifacts = []
        for p in (img, bd / "zephyr" / "zephyr.elf", out / "sbom.cdx.json"):
            if p.exists():
                dest = out / p.name
                if p.resolve() != dest.resolve():
                    shutil.copy2(p, dest)
                artifacts.append(dest)
        sums = out / "SHA256SUMS"
        with open(sums, "w") as f:
            for a in artifacts:
                if a.name == "SHA256SUMS":
                    continue
                h = hashlib.sha256(a.read_bytes()).hexdigest()
                f.write(f"{h}  {a.name}\n")
        log.inf(f"Release in {out}: " + ", ".join(a.name for a in artifacts) + ", SHA256SUMS")
