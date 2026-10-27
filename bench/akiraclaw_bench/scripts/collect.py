#!/usr/bin/env python3
"""
collect.py — AkiraClaw benchmark result harvester

Reads <<BENCH_JSON_START>> / <<BENCH_JSON_END>> delimited JSON objects from
an RTT or UART serial log (file or live serial port) and writes them to a
structured JSON array suitable for paper tables.

Usage:
    # From a saved log file:
    python3 collect.py --input bench_run.log --output results.json

    # Live from serial port (requires pyserial):
    python3 collect.py --port /dev/ttyUSB0 --baud 115200 --output results.json

    # With ELF for accurate flash_tflm_kb (overrides runtime value):
    python3 collect.py --input bench_run.log --elf build/zephyr/zephyr.elf

    # Print a LaTeX table fragment:
    python3 collect.py --input bench_run.log --latex

Options:
    --input FILE      Read from log file
    --port  DEV       Read from serial port (live)
    --baud  N         Baud rate for serial (default: 115200)
    --output FILE     Write JSON results (default: results.json)
    --elf   FILE      Path to zephyr.elf for nm-based size extraction
    --latex           Print LaTeX table rows to stdout after collection
    --timeout N       Serial read timeout in seconds (default: 120)
"""

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

JSON_START = "<<BENCH_JSON_START>>"
JSON_END   = "<<BENCH_JSON_END>>"


# ── Log reader ───────────────────────────────────────────────────────────

def iter_lines_file(path: str):
    with open(path, "r", errors="replace") as f:
        for line in f:
            yield line.rstrip("\n")


def iter_lines_serial(port: str, baud: int, timeout: int):
    try:
        import serial
    except ImportError:
        print("ERROR: pyserial not installed. Run: pip install pyserial", file=sys.stderr)
        sys.exit(1)

    print(f"Listening on {port} @ {baud} bps (timeout={timeout}s) ...", file=sys.stderr)
    deadline = time.monotonic() + timeout
    buf = ""

    with serial.Serial(port, baud, timeout=0.1) as ser:
        while time.monotonic() < deadline:
            chunk = ser.read(256).decode("ascii", errors="replace")
            if not chunk:
                continue
            buf += chunk
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                yield line.rstrip("\r")
            deadline = time.monotonic() + timeout  # reset on any activity


# ── JSON extractor ───────────────────────────────────────────────────────

def extract_json_objects(lines):
    """Yield parsed JSON dicts from sentinel-delimited blocks.

    Handles both multi-line blocks (START and END on separate lines) and
    single-line blocks where the WASM module emits everything on one line.
    """
    in_block = False
    block_lines = []

    for line in lines:
        # Single-line case: both START and END on same line
        if JSON_START in line and JSON_END in line:
            after_start = line.split(JSON_START, 1)[1]
            text = after_start.split(JSON_END, 1)[0].strip()
            # Remove log prefixes from inlined text
            text = re.sub(r"^\[[\d:.,]+\]\s*<\w+>\s*\S+:\s*", "", text)
            try:
                obj = json.loads(text)
                yield obj
            except json.JSONDecodeError as e:
                print(f"WARN: JSON parse error (single-line): {e}\nText: {text[:200]}",
                      file=sys.stderr)
            continue

        if JSON_START in line:
            in_block = True
            block_lines = []
            # In case there's content after the START marker on the same line
            after = line.split(JSON_START, 1)[1].strip()
            if after:
                block_lines.append(after)
            continue
        if JSON_END in line:
            if in_block and block_lines:
                text = "\n".join(block_lines)
                try:
                    obj = json.loads(text)
                    yield obj
                except json.JSONDecodeError as e:
                    print(f"WARN: JSON parse error: {e}\nBlock: {text[:200]}",
                          file=sys.stderr)
            in_block = False
            block_lines = []
            continue
        if in_block:
            # Strip leading log prefixes like "[00:00:01.234,567] <inf>"
            clean = re.sub(r"^\[[\d:.,]+\]\s*<\w+>\s*\S+:\s*", "", line)
            block_lines.append(clean)


# ── ELF analysis ─────────────────────────────────────────────────────────

def elf_tflm_size_kb(elf_path: str) -> Optional[int]:
    """Use nm to find __tflm_text_start / __tflm_text_end and compute size."""
    try:
        result = subprocess.run(
            ["arm-none-eabi-nm", "--print-size", "--numeric-sort", elf_path],
            capture_output=True, text=True, timeout=30
        )
        syms = {}
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 2:
                sym = parts[-1]
                addr = int(parts[0], 16)
                if sym in ("__tflm_text_start", "__tflm_text_end"):
                    syms[sym] = addr
        if "__tflm_text_start" in syms and "__tflm_text_end" in syms:
            size = syms["__tflm_text_end"] - syms["__tflm_text_start"]
            return max(0, (size + 1023) // 1024)
    except (subprocess.TimeoutExpired, FileNotFoundError, ValueError):
        pass
    return None


def elf_section_sizes(elf_path: str) -> dict:
    """Run arm-none-eabi-size and return section sizes."""
    try:
        result = subprocess.run(
            ["arm-none-eabi-size", "-A", elf_path],
            capture_output=True, text=True, timeout=30
        )
        sizes = {}
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[0].startswith("."):
                try:
                    sizes[parts[0]] = int(parts[1])
                except ValueError:
                    pass
        return sizes
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return {}


# ── Post-processing / enrichment ─────────────────────────────────────────

def enrich(records: list, elf_path: Optional[str]) -> list:
    """Add/correct fields using ELF analysis."""
    if not elf_path:
        return records

    tflm_kb = elf_tflm_size_kb(elf_path)
    sizes   = elf_section_sizes(elf_path)

    for rec in records:
        if rec.get("bench") == "B4":
            if tflm_kb is not None:
                rec["flash_tflm_kb"] = tflm_kb
                rec["flash_tflm_source"] = "elf_nm"
                rec["mock_mode"] = False
                rec.pop("mock_reason", None)
            if ".text" in sizes:
                rec["elf_text_total_kb"] = (sizes[".text"] + 1023) // 1024
            if ".bss" in sizes:
                rec["elf_bss_kb"] = (sizes[".bss"] + 1023) // 1024

    return records


# ── LaTeX output ─────────────────────────────────────────────────────────

B1_CONFIGS = ["native_tflm", "wasm_noguard", "wasm_claw", "wasm_claw_mpu"]

def print_latex(records: list):
    print("\n% ── B1 Inference Latency (µs) ──")
    print(r"\begin{tabular}{lrrrrr}")
    print(r"  Config & load & avg & p99 & max & unload \\")
    print(r"  \hline")
    b1 = {r["config"]: r for r in records if r.get("bench") == "B1"}
    for cfg in B1_CONFIGS:
        if cfg in b1:
            r = b1[cfg]
            mock = " (mock)" if r.get("mock_mode") else ""
            print(f"  {cfg}{mock} & {r.get('load_us',0)} & {r.get('run_avg_us',0)} & "
                  f"{r.get('run_p99_us',0)} & {r.get('run_max_us',0)} & "
                  f"{r.get('unload_us',0)} \\\\")
    print(r"\end{tabular}")

    print("\n% ── B2 MPU domain switch (ns) ──")
    b2 = [r for r in records if r.get("bench") == "B2"]
    if b2:
        r = b2[0]
        mock = " (mock)" if r.get("mock_mode") else ""
        print(f"%  begin: avg={r.get('sandbox_exec_begin_avg_ns',0)} "
              f"p99={r.get('sandbox_exec_begin_p99_ns',0)} ns{mock}")
        print(f"%  end:   avg={r.get('sandbox_exec_end_avg_ns',0)} "
              f"p99={r.get('sandbox_exec_end_p99_ns',0)} ns{mock}")

    print("\n% ── B3 Audit log (ns) ──")
    for r in records:
        if r.get("bench") == "B3":
            hmac = "HMAC" if r.get("hmac_enabled") else "no-HMAC"
            print(f"%  {hmac}: avg={r.get('audit_write_avg_ns',0)} "
                  f"p99={r.get('audit_write_p99_ns',0)} ns")

    print("\n% ── B4 Memory ──")
    for r in records:
        if r.get("bench") == "B4":
            print(f"%  TFLM flash: {r.get('flash_tflm_kb',0)} KB")
            print(f"%  Arena RAM:  {r.get('ram_arena_kb',0)} KB")
            print(f"%  sandbox_ctx: {r.get('sandbox_ctx_sizeof',0)} B")
            print(f"%  DRAM reclaim: {r.get('ram_dram_reclaimed_bytes',0)} B")

    print("\n% ── B5 BLE coexistence (µs) ──")
    for r in records:
        if r.get("bench") == "B5":
            print(f"%  BLE off: {r.get('infer_avg_us_ble_off',0)} µs avg")
            print(f"%  BLE on:  avg={r.get('infer_avg_us',0)} "
                  f"p99={r.get('infer_p99_us',0)} µs | "
                  f"missed_adv={r.get('missed_adv_events',0)}/{r.get('total_adv_windows',0)}")


# ── Main ─────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="AkiraClaw benchmark result collector")
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument("--input",   metavar="FILE", help="Log file to parse")
    src.add_argument("--port",    metavar="DEV",  help="Serial port device")
    parser.add_argument("--baud",    type=int, default=115200)
    parser.add_argument("--timeout", type=int, default=120, help="Serial idle timeout (s)")
    parser.add_argument("--output",  default="results.json", help="Output JSON file")
    parser.add_argument("--elf",     metavar="FILE", help="zephyr.elf for size analysis")
    parser.add_argument("--latex",   action="store_true", help="Print LaTeX table")
    args = parser.parse_args()

    if args.input:
        lines = iter_lines_file(args.input)
    else:
        lines = iter_lines_serial(args.port, args.baud, args.timeout)

    records = list(extract_json_objects(lines))

    if not records:
        print("ERROR: no benchmark JSON objects found in input", file=sys.stderr)
        sys.exit(1)

    print(f"Collected {len(records)} result(s)", file=sys.stderr)

    records = enrich(records, args.elf)

    out = Path(args.output)
    out.write_text(json.dumps(records, indent=2))
    print(f"Written: {out}", file=sys.stderr)

    if args.latex:
        print_latex(records)

    # Print summary to stderr
    for r in records:
        bench = r.get("bench", "?")
        mock  = " [MOCK]" if r.get("mock_mode") else ""
        if bench == "B1":
            print(f"  B1/{r.get('config','?')}: avg={r.get('run_avg_us',0)} µs{mock}",
                  file=sys.stderr)
        elif bench == "B2":
            print(f"  B2: begin_avg={r.get('sandbox_exec_begin_avg_ns',0)} ns{mock}",
                  file=sys.stderr)
        elif bench == "B3":
            hmac = "HMAC" if r.get("hmac_enabled") else "no-HMAC"
            print(f"  B3 ({hmac}): avg={r.get('audit_write_avg_ns',0)} ns{mock}",
                  file=sys.stderr)
        elif bench == "B4":
            print(f"  B4: tflm={r.get('flash_tflm_kb',0)} KB "
                  f"arena={r.get('ram_arena_kb',0)} KB{mock}",
                  file=sys.stderr)
        elif bench == "B5":
            print(f"  B5: ble_off={r.get('infer_avg_us_ble_off',0)} "
                  f"ble_on={r.get('infer_avg_us',0)} µs{mock}",
                  file=sys.stderr)


if __name__ == "__main__":
    main()
