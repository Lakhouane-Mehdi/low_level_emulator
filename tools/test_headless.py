#!/usr/bin/env python3
"""Headless ROM regression suite.

Runs `chip8_run` against a fixed set of (ROM, args, expected hash) tuples
and reports any mismatches. Run from the repo root after building Release:

    python tools/test_headless.py
    python tools/test_headless.py --update     # refresh hashes (dangerous)

Exit 0 = all match, 1 = any mismatch, 2 = runner missing.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNNER_CANDIDATES = [
    ROOT / "build" / "Release" / "chip8_run.exe",
    ROOT / "build" / "chip8_run",
    ROOT / "build" / "Debug" / "chip8_run.exe",
]


# (label, rom path, extra args, frames, expected_hash_or_None)
# expected_hash=None means "we don't have a golden value yet — record one".
# When you flip --update on, mismatches become the new golden value.
GOLDEN: list[tuple[str, str, list[str], int, str | None]] = [
    # IBM logo: stable across all CHIP-8 quirks. Best smoke test.
    ("ibm_logo_60f",      "roms/IBM_Logo.ch8",  [],                    60,  "0xFA6CE8F707AC1F88"),
    ("ibm_logo_seeded",   "roms/IBM_Logo.ch8",  ["--seed", "42"],      60,  "0xFA6CE8F707AC1F88"),

    # Timendus test suite — all ship with deterministic output.
    # Captured on commit 76f3296 with default modern quirks.
    ("test_chip8_logo",   "roms/tests/1-chip8-logo.ch8", [],           120, "0x7DE30E233B46217A"),
    ("test_ibm_logo",     "roms/tests/2-ibm-logo.ch8",   [],           120, "0xC712424447ADF9F2"),
    ("test_corax",        "roms/tests/3-corax+.ch8",     [],           300, "0x448F8429E8F2E596"),
    ("test_flags",        "roms/tests/4-flags.ch8",      [],           300, "0x8F64FF536A3BB69F"),

    # MX-8 demo halts at frame ~2 with EXIT — exit code 2 expected.
    # Hash captures the post-halt framebuffer (blank — no DRW in the demo).
    ("mx8_demo",          "roms/mx8_demo.ch8",  ["--mx8"],             5,   "0x86062D4C200833DF"),
]

HASH_RE = re.compile(r"hash=(0x[0-9A-Fa-f]+)")


def find_runner() -> Path | None:
    for c in RUNNER_CANDIDATES:
        if c.exists():
            return c
    return None


def run_case(runner: Path, rom: str, extra: list[str], frames: int) -> tuple[int, str | None]:
    cmd = [str(runner), rom, "--frames", str(frames), "--print-hash", *extra]
    res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    out = (res.stdout or "") + (res.stderr or "")
    m = HASH_RE.search(out)
    h = m.group(1).upper().replace("0X", "0x") if m else None
    return res.returncode, h


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true",
                    help="Print fresh hashes for all cases (does not modify the source).")
    args = ap.parse_args()

    runner = find_runner()
    if not runner:
        print("error: chip8_run not found. Build with cmake --build build --config Release",
              file=sys.stderr)
        return 2

    print(f"runner: {runner.relative_to(ROOT)}")
    fails = 0
    fresh: list[str] = []

    for label, rom, extra, frames, expected in GOLDEN:
        rom_path = ROOT / rom
        if not rom_path.exists():
            print(f"skip  {label:20s}  (missing rom: {rom})")
            continue
        rc, got = run_case(runner, rom, extra, frames)
        if got is None:
            print(f"FAIL  {label:20s}  (no hash in output, rc={rc})")
            fails += 1
            continue

        if expected is None or args.update:
            fresh.append(f'  ("{label}", "{rom}", {extra!r}, {frames}, "{got}"),')
            if expected is None:
                print(f"new   {label:20s}  rc={rc}  hash={got}")
            else:
                ok = (got.lower() == expected.lower())
                print(f"{'ok  ' if ok else 'CHG '}{label:20s}  rc={rc}  hash={got}  was={expected}")
        else:
            ok = (got.lower() == expected.lower())
            mark = "ok  " if ok else "FAIL"
            print(f"{mark}  {label:20s}  rc={rc}  hash={got}")
            if not ok:
                print(f"      expected {expected}")
                fails += 1

    if args.update or any(e is None for _, _, _, _, e in GOLDEN):
        print("\n--- proposed GOLDEN (paste into tools/test_headless.py) ---")
        print("GOLDEN = [")
        for line in fresh:
            print(line)
        print("]")

    # Replay regression: every .json under replays/ must round-trip with
    # exit 0 (= all checkpoints pass). One drift = entire CI run fails.
    replays_dir = ROOT / "replays"
    if replays_dir.exists():
        print()
        for path in sorted(replays_dir.glob("*.json")):
            cmd = [str(runner), "--replay", str(path)]
            res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            label = path.name
            if res.returncode == 0:
                print(f"ok    replay {label}")
            else:
                print(f"FAIL  replay {label}  rc={res.returncode}")
                print((res.stderr or "").rstrip())
                fails += 1

    if fails:
        print(f"\n{fails} FAILURE(S)")
        return 1
    print("\nALL PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
