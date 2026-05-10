#!/usr/bin/env python3
"""Author a synthetic replay file by hand and write it as JSON.

Used as a smoke test for the replay pipeline: produces a replay that
chip8_run can load + execute + checkpoint-verify, without needing a
human at the GUI keyboard.

Usage:
    python tools/synth_replay.py <rom> --frames 60 --out replay.json [--seed N]
"""
from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

DEFAULT_QUIRKS = {
    "shift_in_place":     True,
    "load_store_no_inc":  True,
    "jump_with_offset_x": True,
    "vf_reset":           False,
    "display_wait":       False,
    "clip_sprites":       True,
    "mx8_extensions":     False,
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--seed",   type=int, default=42)
    ap.add_argument("--out",    type=Path, required=True)
    ap.add_argument("--isa",    default="mx8")
    args = ap.parse_args()

    rom_bytes = Path(args.rom).read_bytes()
    rom_hex = rom_bytes.hex()

    # Build a minimal replay with one tap-style event sequence: press key 5
    # at frame 5, release at frame 10. Sprinkle a checkpoint at the final
    # frame whose hash will be filled in from a probe run below.
    replay = {
        "format":         "chip8-replay",
        "version":        1,
        "rom_bytes_hex":  rom_hex,
        "isa":            args.isa,
        "quirks":         DEFAULT_QUIRKS,
        "seed":           args.seed,
        "events": [
            { "frame": 5,  "type": "InjectKey", "key": 5, "down": True  },
            { "frame": 10, "type": "InjectKey", "key": 5, "down": False },
        ],
        "checkpoints":    [],
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(replay, indent=2))
    print(f"wrote synthetic replay -> {args.out}")

    # Probe run to discover the final hash, then update the file with a
    # checkpoint so future runs can validate against it.
    runner = ROOT / "build" / "Release" / "chip8_run.exe"
    if not runner.exists():
        print("note: chip8_run.exe not built; replay has no checkpoint")
        return 0

    probe = subprocess.run(
        [str(runner), "--replay", str(args.out), "--frames", str(args.frames),
         "--print-hash"],
        capture_output=True, text=True, cwd=ROOT)
    print("probe stdout:", probe.stdout.strip())
    print("probe stderr:", probe.stderr.strip())
    if probe.returncode != 0:
        print(f"probe exit={probe.returncode} — leaving replay without checkpoint")
        return 1

    h = None
    for line in probe.stdout.splitlines():
        if line.startswith("hash="):
            h = line.split("=", 1)[1].strip()
            break
    if not h:
        print("could not read hash from probe output")
        return 1

    replay["checkpoints"].append({
        "frame": args.frames - 1,
        "hash":  h,
    })
    args.out.write_text(json.dumps(replay, indent=2))
    print(f"updated replay with checkpoint @ frame {args.frames - 1}: {h}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
