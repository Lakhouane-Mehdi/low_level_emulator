#!/usr/bin/env python3
"""End-to-end test for chip8_run --diff-replay.

Authors a synthetic baseline replay, then creates two tampered variants:
  (1) seed change only      -> divergence detected at frame 0 (config-time)
  (2) extra event at frame 50 -> divergence detected at frame 50 (runtime)

For each, asserts diff-replay's exit code AND the divergence frame it
reports. Also runs --self-check on the original to verify it passes.

Run from repo root:
    python tools/test_diff_replay.py

Exit 0 = all pass, 1 = any failure, 2 = runner missing.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "build" / "Release" / "chip8_run.exe"
ROM    = ROOT / "roms" / "BREAKOUT.ch8"

BASELINE_JSON = ROOT / "replays" / "_diff_test_baseline.json"
TAMPER_SEED   = ROOT / "replays" / "_diff_test_seed.json"
TAMPER_EVENT  = ROOT / "replays" / "_diff_test_event.json"

DIVERGENCE_RE = re.compile(r"divergence at frame (\d+)")

DEFAULT_QUIRKS = {
    "shift_in_place":     True,
    "load_store_no_inc":  True,
    "jump_with_offset_x": True,
    "vf_reset":           False,
    "display_wait":       False,
    "clip_sprites":       True,
    "mx8_extensions":     False,
}

g_failures = 0


def fail(msg: str) -> None:
    global g_failures
    g_failures += 1
    print(f"FAIL: {msg}")


def ok(msg: str) -> None:
    print(f"ok   {msg}")


def write_baseline() -> None:
    rom = ROM.read_bytes()
    replay = {
        "format":         "chip8-replay",
        "version":        1,
        "rom_bytes_hex":  rom.hex(),
        "isa":            "mx8",
        "quirks":         DEFAULT_QUIRKS,
        "seed":           42,
        "events": [
            { "frame": 5,  "type": "InjectKey", "key": 5, "down": True  },
            { "frame": 10, "type": "InjectKey", "key": 5, "down": False },
        ],
        "checkpoints":    [],
    }
    BASELINE_JSON.parent.mkdir(parents=True, exist_ok=True)
    BASELINE_JSON.write_text(json.dumps(replay, indent=2))


def diff_replay(a: Path, b: Path, force: bool = False) -> tuple[int, str]:
    cmd = [str(RUNNER), "--diff-replay", str(a), str(b)]
    if force: cmd.append("--force")
    res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return res.returncode, (res.stdout or "") + (res.stderr or "")


def self_check(p: Path) -> tuple[int, str]:
    cmd = [str(RUNNER), "--replay", str(p), "--self-check"]
    res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return res.returncode, (res.stdout or "") + (res.stderr or "")


def parse_divergence_frame(out: str) -> int | None:
    m = DIVERGENCE_RE.search(out)
    return int(m.group(1)) if m else None


def main() -> int:
    if not RUNNER.exists():
        print(f"runner not found: {RUNNER}", file=sys.stderr)
        return 2
    if not ROM.exists():
        print(f"rom not found: {ROM}", file=sys.stderr)
        return 2

    write_baseline()

    # Test 1: replay vs itself -> exit 0, no divergence in output.
    rc, out = diff_replay(BASELINE_JSON, BASELINE_JSON)
    if rc == 0 and "no execution divergence" in out:
        ok("self-diff exits 0 with no divergence")
    else:
        fail(f"self-diff: rc={rc}\n{out}")

    # Test 2: tamper with seed -> divergence at frame 0 (rng component).
    base = json.loads(BASELINE_JSON.read_text())
    seed_d = dict(base); seed_d["seed"] = 999
    TAMPER_SEED.write_text(json.dumps(seed_d, indent=2))
    rc, out = diff_replay(BASELINE_JSON, TAMPER_SEED)
    df = parse_divergence_frame(out)
    if rc == 1 and df == 0 and "rng" in out:
        ok(f"seed tamper -> divergence at frame {df}, rng component flagged")
    else:
        fail(f"seed tamper: rc={rc}, frame={df}, expected (1, 0, rng in output)\n{out}")

    # Test 3: insert a runtime event at frame 50 -> divergence at frame 50
    # (use --force because input streams differ in length).
    ev_d = json.loads(json.dumps(base))   # deep copy
    ev_d["events"].append({"frame": 50, "type": "WriteMemory", "addr": 0x300, "value": 0xAB})
    ev_d["events"].sort(key=lambda e: e["frame"])
    TAMPER_EVENT.write_text(json.dumps(ev_d, indent=2))
    rc, out = diff_replay(BASELINE_JSON, TAMPER_EVENT, force=True)
    df = parse_divergence_frame(out)
    if rc == 1 and df == 50 and "memory" in out:
        ok(f"event tamper -> divergence at frame {df}, memory component flagged")
    else:
        fail(f"event tamper: rc={rc}, frame={df}, expected (1, 50, memory in output)\n{out}")

    # Test 4: input-divergence detection without --force -> refuse.
    rc, out = diff_replay(BASELINE_JSON, TAMPER_EVENT)
    if rc == 1 and "input streams differ" in out and "Pass --force" in out:
        ok("input-divergence detected and refused without --force")
    else:
        fail(f"input-divergence refusal: rc={rc}\n{out}")

    # Test 5: --self-check on a checkpoint-bearing replay.
    # First, compute and embed a real checkpoint.
    probe = subprocess.run(
        [str(RUNNER), "--replay", str(BASELINE_JSON),
         "--frames", "60", "--print-hash"],
        cwd=ROOT, capture_output=True, text=True)
    h = None
    for line in (probe.stdout or "").splitlines():
        if line.startswith("hash="):
            h = line.split("=", 1)[1].strip()
    if h:
        base["checkpoints"] = [{"frame": 59, "hash": h}]
        BASELINE_JSON.write_text(json.dumps(base, indent=2))
        rc, out = self_check(BASELINE_JSON)
        if rc == 0 and "ok  " in out:
            ok("self-check passes on baseline replay")
        else:
            fail(f"self-check: rc={rc}\n{out}")

    # Cleanup
    for p in (BASELINE_JSON, TAMPER_SEED, TAMPER_EVENT):
        try: p.unlink()
        except FileNotFoundError: pass

    if g_failures:
        print(f"\n{g_failures} FAILURE(S)")
        return 1
    print("\nALL PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
