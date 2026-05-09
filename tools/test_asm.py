#!/usr/bin/env python3
"""Smoke test: assemble a snippet covering every mnemonic, then verify each
emitted opcode matches what we expect by hand.

Run with: python tools/test_asm.py
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from asm import Assembler, AsmError

# (source, expected hex sequence)
CASES = [
    # core 35 ops + SCHIP
    ("CLS",           "00E0"),
    ("RET",           "00EE"),
    ("JP 0x234",      "1234"),
    ("CALL 0x300",    "2300"),
    ("SE V1, 0x42",   "3142"),
    ("SNE V1, 0x42",  "4142"),
    ("SE V1, V2",     "5120"),
    ("LD V3, 0x12",   "6312"),
    ("ADD V3, 0x10",  "7310"),
    ("LD V0, V1",     "8010"),
    ("OR V0, V1",     "8011"),
    ("AND V0, V1",    "8012"),
    ("XOR V0, V1",    "8013"),
    ("ADD V0, V1",    "8014"),
    ("SUB V0, V1",    "8015"),
    ("SHR V0",        "8006"),
    ("SHR V0, V1",    "8016"),
    ("SUBN V0, V1",   "8017"),
    ("SHL V0",        "800E"),
    ("SHL V0, V1",    "801E"),
    ("SNE V1, V2",    "9120"),
    ("LD I, 0x250",   "A250"),
    ("JP V0, 0x200",  "B200"),
    ("RND V5, 0x0F",  "C50F"),
    ("DRW V1, V2, 5", "D125"),
    ("SKP V3",        "E39E"),
    ("SKNP V3",       "E3A1"),
    ("LD V4, DT",     "F407"),
    ("LD V4, K",      "F40A"),
    ("LD DT, V4",     "F415"),
    ("LD ST, V4",     "F418"),
    ("ADD I, V4",     "F41E"),
    ("LD F, V4",      "F429"),
    ("LD HF, V4",     "F430"),
    ("LD B, V4",      "F433"),
    ("LD [I], V4",    "F455"),
    ("LD V4, [I]",    "F465"),
    ("LD R, V4",      "F475"),
    ("LD V4, R",      "F485"),
    ("EXIT",          "00FD"),
    ("LOW",           "00FE"),
    ("HIGH",          "00FF"),
    ("SCR",           "00FB"),
    ("SCL",           "00FC"),
    ("SCD 3",         "00C3"),
    ("SCU 5",         "00D5"),
    # MX-8 extensions
    ("MUL V1, V2",    "5121"),
    ("DIV V1, V2",    "5122"),
    ("SWAP V1, V2",   "5123"),
    ("MEMCPY 1",      "F050"),
    ("MEMCPY 16",     "FF50"),
    ("MEMSET 4",      "F360"),
    ("RNDSEED 4",     "F370"),
]

def asm_one(src: str) -> bytes:
    a = Assembler(src)
    rom, _ = a.assemble()
    return rom

def main():
    fails = 0
    for src, expected_hex in CASES:
        try:
            rom = asm_one(src)
        except AsmError as e:
            print(f"FAIL  {src!r}: AsmError {e}")
            fails += 1
            continue
        got = rom.hex().upper()
        ok = got == expected_hex
        mark = "ok  " if ok else "FAIL"
        print(f"{mark}  {src:<22} -> {got}   expected {expected_hex}")
        if not ok:
            fails += 1

    # Test labels (forward ref) and .equ
    src = """
        .equ TARGET, 0x250
        JP later
        .org 0x250
later:  LD I, sprite
sprite: .db 0xAB, 0xCD
    """
    a = Assembler(src)
    rom, _ = a.assemble()
    # rom starts at 0x200. JP later -> 1250 ; then padding zeros until 0x250
    # at 0x250: LD I, sprite -> A252 ; at 0x252: AB CD
    h = rom.hex().upper()
    if not h.startswith("1250"):
        print(f"FAIL labels: rom starts {h[:8]}, expected 1250...")
        fails += 1
    else:
        print(f"ok    labels+ org   first 8 bytes: {h[:8]}  total {len(rom)} bytes")

    # Test that 0x250 - 0x200 = 80 bytes of zeros + the actual code.
    if len(rom) != 80 + 4:
        print(f"FAIL labels: expected 84 bytes, got {len(rom)}")
        fails += 1
    elif rom[80:84] != bytes([0xA2, 0x52, 0xAB, 0xCD]):
        print(f"FAIL labels: tail bytes {rom[80:84].hex()}, expected A252ABCD")
        fails += 1
    else:
        print("ok    label+org tail bytes correct")

    if fails:
        print(f"\n{fails} FAILURE(S)")
        sys.exit(1)
    else:
        print("\nall asm self-tests passed")

if __name__ == "__main__":
    main()
