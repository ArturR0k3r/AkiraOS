#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the AkiraOS WASM import ABI for drift between firmware and SDK.

The firmware exports a set of native functions to WASM apps (the "env" import
module and a few others). The AkiraSDK header declares the matching imports.
This script extracts both lists and fails if they disagree, except for the
mismatches recorded in scripts/wasm_abi_allowlist.txt (which exist because some
natives live only on product branches, or the SDK is ahead of this firmware).

    scripts/check_wasm_abi.py [--sdk-root AkiraSDK] [--update-allowlist]

Firmware side: names in AKIRA_NATIVE_API_DEFINE() tables under src/.
SDK side: top-level `extern` function declarations in <sdk>/include/akira_api.h.
Both are parsed textually; no toolchain is required.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ALLOWLIST = REPO / "scripts" / "wasm_abi_allowlist.txt"

# {"name", (void *)fn, "sig", NULL} entries inside AKIRA_NATIVE_API_DEFINE tables.
ENTRY_RE = re.compile(r'\{\s*"([A-Za-z_]\w*)"\s*,\s*\(void\s*\*\)')
# A native table is one declared right before an AKIRA_NATIVE_API_DEFINE.
TABLE_RE = re.compile(
    r'static\s+const\s+NativeSymbol\s+(\w+)\[\]\s*=\s*\{(.*?)\};',
    re.S,
)
DEFINE_RE = re.compile(r'AKIRA_NATIVE_API_DEFINE(?:_FLAGS)?\s*\(\s*\w+\s*,\s*"([^"]+)"\s*,\s*(\w+)')
# extern <type> name(...) in the SDK header, ignoring static inline wrappers.
SDK_DECL_RE = re.compile(r'^\s*extern\s+[\w\s\*]+?\b([A-Za-z_]\w*)\s*\(', re.M)


def strip_comments(text: str) -> str:
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//.*', '', text)
    return text


def firmware_symbols() -> set:
    names = set()
    for path in (REPO / "src").rglob("*.c"):
        text = strip_comments(path.read_text(errors="ignore"))
        tables = {name: body for name, body in
                  ((m.group(1), m.group(2)) for m in TABLE_RE.finditer(text))}
        for m in DEFINE_RE.finditer(text):
            module, table = m.group(1), m.group(2)
            if module != "env" or table not in tables:
                continue
            names.update(ENTRY_RE.findall(tables[table]))
    return names


def sdk_symbols(sdk_root: Path) -> set:
    header = sdk_root / "include" / "akira_api.h"
    if not header.is_file():
        sys.exit(f"error: SDK header not found: {header}")
    text = strip_comments(header.read_text(errors="ignore"))
    return set(SDK_DECL_RE.findall(text))


def load_allowlist() -> set:
    if not ALLOWLIST.is_file():
        return set()
    return {line.split("#", 1)[0].strip()
            for line in ALLOWLIST.read_text().splitlines()
            if line.split("#", 1)[0].strip()}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sdk-root", default=str(REPO / "AkiraSDK"), type=Path)
    ap.add_argument("--update-allowlist", action="store_true",
                    help="record the current mismatches instead of failing")
    args = ap.parse_args()

    fw = firmware_symbols()
    sdk = sdk_symbols(args.sdk_root)
    if not fw:
        sys.exit("error: no firmware natives found; is the AKIRA_NATIVE_API_DEFINE parse working?")

    sdk_only = sorted(sdk - fw)     # SDK declares an import the firmware does not export
    fw_only = sorted(fw - sdk)      # firmware exports a native the SDK does not declare
    mismatches = {f"sdk-only:{n}" for n in sdk_only} | {f"fw-only:{n}" for n in fw_only}

    if args.update_allowlist:
        ALLOWLIST.write_text(
            "# SPDX-License-Identifier: Apache-2.0\n"
            "# Known WASM ABI mismatches between this firmware and AkiraSDK/include/akira_api.h.\n"
            "# 'sdk-only:<name>' — declared in the SDK, not exported by this firmware\n"
            "#   (usually a product-branch native, or the SDK is ahead).\n"
            "# 'fw-only:<name>'  — exported by this firmware, not declared in the SDK.\n"
            "# Regenerate with: scripts/check_wasm_abi.py --update-allowlist\n"
            + "".join(f"{m}\n" for m in sorted(mismatches)))
        print(f"wrote {len(mismatches)} entries to {ALLOWLIST}")
        return 0

    allowed = load_allowlist()
    new = sorted(mismatches - allowed)
    stale = sorted(allowed - mismatches)

    if new:
        print("error: new WASM ABI drift (add to scripts/wasm_abi_allowlist.txt only if intended):")
        for m in new:
            print(f"  {m}")
    if stale:
        print("note: allowlist entries that no longer apply (remove them):")
        for m in stale:
            print(f"  {m}")
    if new:
        return 1
    print(f"WASM ABI OK: {len(fw)} firmware natives, {len(sdk)} SDK imports, "
          f"{len(mismatches & allowed)} known mismatches.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
