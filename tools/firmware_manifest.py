#!/usr/bin/env python3
"""Add a signed firmware image to the OTA catalogue the Akira app reads.

    tools/firmware_manifest.py build/zephyr/zephyr.signed.bin 1.6.2 "Fixes BLE pairing" --out dist

Writes dist/firmware/akiraos-<version>.bin and merges the build into
dist/firmware.json, then prints the R2 upload commands.
"""
import argparse
import hashlib
import json
import shutil
from datetime import date
from pathlib import Path

BASE_URL = "https://console.app.akiraos.dev/catalog"
BUCKET = "akiraconsoleapp"

ap = argparse.ArgumentParser()
ap.add_argument("image", type=Path, help="zephyr.signed.bin")
ap.add_argument("version")
ap.add_argument("changelog", nargs="?", default="")
ap.add_argument("--out", type=Path, default=Path("dist"))
args = ap.parse_args()

data = args.image.read_bytes()
name = f"akiraos-{args.version}.bin"
(args.out / "firmware").mkdir(parents=True, exist_ok=True)
shutil.copyfile(args.image, args.out / "firmware" / name)

manifest_path = args.out / "firmware.json"
manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {"builds": []}
manifest["builds"] = [b for b in manifest["builds"] if b["version"] != args.version]
manifest["builds"].append({
    "version": args.version,
    "url": f"{BASE_URL}/firmware/{name}",
    "size": len(data),
    "sha256": hashlib.sha256(data).hexdigest(),
    "changelog": args.changelog,
    "date": date.today().isoformat(),
})
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

print(f"wrangler r2 object put {BUCKET}/firmware/{name} --file {args.out / 'firmware' / name}")
print(f"wrangler r2 object put {BUCKET}/firmware.json --file {manifest_path} --content-type application/json")
