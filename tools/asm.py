#!/usr/bin/env python3
"""
CHIP-8 / SUPER-CHIP / MX-8 / XO-CHIP assembler.

Two-pass: pass 1 sizes every line + records labels; pass 2 emits bytes.
Output is a raw .ch8 ROM ready to load at 0x200.

Syntax (case-insensitive for mnemonics; labels case-sensitive):
    label:
    ; comment
    LD V0, 0x05         ; immediate
    LD I, sprite
    DRW V0, V1, 5
    JP main_loop
    .org 0x300          ; explicit address (rare; default 0x200)
    .db  0xF0,0x90,0x90,0x90,0xF0  ; raw bytes
    .dw  0x1234         ; raw word, big-endian
    .ascii "hello"      ; ascii bytes
    .equ NAME, value    ; constant alias

Standard CHIP-8 mnemonics (35 ops) + SUPER-CHIP extensions:
    SCD n  SCU n  SCR  SCL  EXIT  LOW  HIGH
    LD HF, Vx     LD R, Vx     LD Vx, R   (FX30 / FX75 / FX85)

MX-8 extensions:
    MUL Vx,Vy   DIV Vx,Vy   SWAP Vx,Vy   MEMCPY n   MEMSET n   RNDSEED n

XO-CHIP extensions:
    PLANE n          select draw-plane bitmask 0..3        (FN01)
    SAVE Vx-Vy       store register range to [I]           (5XY2)
    LOAD Vx-Vy       load  register range from [I]         (5XY3)
    AUDIO            load 16-byte audio pattern from [I]    (F002)
    PITCH Vx         set audio playback pitch               (FX3A)
    LDLONG addr      i := long addr (4-byte 16-bit load)    (F000 NNNN)
    (SCD/SCU/SCR/SCL are plane-aware under the XO-CHIP ISA.)

Numbers: decimal (12), hex (0x12 / $12), binary (0b1010 / %1010).
Vx registers: V0..VF (case-insensitive).

Run:
    python tools/asm.py demo.asm -o demo.ch8
    python tools/asm.py demo.asm --listing      # also print listing
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


class AsmError(Exception):
    def __init__(self, msg: str, lineno: int = 0, line: str = ""):
        super().__init__(msg)
        self.lineno = lineno
        self.line = line


# --------- lexing helpers ---------

_NUM_RE = re.compile(r"^(?:0x([0-9a-fA-F]+)|\$([0-9a-fA-F]+)|0b([01]+)|%([01]+)|(\d+))$")
_VREG_RE = re.compile(r"^[Vv]([0-9a-fA-F])$")
_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def parse_number(tok: str, equs: dict[str, int]) -> int:
    """Parse a literal or named constant. Raises ValueError on failure."""
    tok = tok.strip()
    if tok in equs:
        return equs[tok]
    m = _NUM_RE.match(tok)
    if not m:
        raise ValueError(f"not a number: {tok!r}")
    if m.group(1) is not None: return int(m.group(1), 16)
    if m.group(2) is not None: return int(m.group(2), 16)
    if m.group(3) is not None: return int(m.group(3), 2)
    if m.group(4) is not None: return int(m.group(4), 2)
    return int(m.group(5), 10)


def parse_vreg(tok: str) -> int:
    m = _VREG_RE.match(tok.strip())
    if not m:
        raise ValueError(f"not a V register: {tok!r}")
    return int(m.group(1), 16)


def is_vreg(tok: str) -> bool:
    return bool(_VREG_RE.match(tok.strip()))


def split_operands(rest: str) -> list[str]:
    if not rest.strip():
        return []
    # Handle ".ascii \"...\"" and similar by not splitting inside quotes.
    out: list[str] = []
    cur = ""
    in_str = False
    for ch in rest:
        if ch == '"':
            in_str = not in_str
            cur += ch
        elif ch == "," and not in_str:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def strip_comment(line: str) -> str:
    in_str = False
    for i, ch in enumerate(line):
        if ch == '"':
            in_str = not in_str
        elif ch == ";" and not in_str:
            return line[:i]
    return line


# --------- tokenized line ---------

@dataclass
class Line:
    lineno: int
    raw: str
    label: Optional[str] = None
    op: Optional[str] = None
    operands: list[str] = field(default_factory=list)
    addr: int = 0
    size: int = 0
    bytes_: bytes = b""


def tokenize(src: str) -> list[Line]:
    lines: list[Line] = []
    for i, raw in enumerate(src.splitlines(), start=1):
        text = strip_comment(raw).rstrip()
        if not text.strip():
            continue
        ln = Line(lineno=i, raw=raw)

        # label?  one or more identifiers ending with ':'
        # support both `label:` on its own line and `label: OP ...`
        text_l = text.lstrip()
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*)$", text_l)
        if m:
            ln.label = m.group(1)
            text_l = m.group(2)
        # mnemonic + operands
        if text_l.strip():
            parts = text_l.split(None, 1)
            ln.op = parts[0].upper() if not parts[0].startswith(".") else parts[0].lower()
            ln.operands = split_operands(parts[1] if len(parts) > 1 else "")
        lines.append(ln)
    return lines


# --------- encoding helpers ---------

def chk_nibble(name: str, v: int) -> int:
    if not (0 <= v <= 0xF):
        raise AsmError(f"{name} out of range (0..F): {v:#x}")
    return v


def chk_byte(name: str, v: int) -> int:
    if not (0 <= v <= 0xFF):
        raise AsmError(f"{name} out of range (0..FF): {v:#x}")
    return v


def chk_addr(name: str, v: int) -> int:
    if not (0 <= v <= 0xFFF):
        raise AsmError(f"{name} out of range (0..FFF): {v:#x}")
    return v


def emit(hi: int, lo: int) -> bytes:
    return bytes([hi & 0xFF, lo & 0xFF])


# --------- mnemonic table (sized) ---------

# Each handler returns size in pass 1 (when bytes aren't yet resolvable due to
# forward labels) and bytes in pass 2 (when labels is the final symbol table).
# Directives can produce variable-size output and are sized in pass 1.

class Assembler:
    def __init__(self, src: str, source_name: str = "<input>"):
        self.src = src
        self.source_name = source_name
        self.lines = tokenize(src)
        self.symbols: dict[str, int] = {}
        self.equs: dict[str, int] = {}
        self.start_address = 0x200

    def err(self, ln: Line, msg: str) -> AsmError:
        return AsmError(f"{self.source_name}:{ln.lineno}: {msg}\n  {ln.raw}",
                        ln.lineno, ln.raw)

    # ---- pass 1: assign addresses ----
    def pass1(self) -> None:
        addr = self.start_address
        for ln in self.lines:
            ln.addr = addr
            if ln.label:
                if ln.label in self.symbols:
                    raise self.err(ln, f"duplicate label: {ln.label}")
                self.symbols[ln.label] = addr

            if ln.op is None:
                continue

            try:
                if ln.op == ".org":
                    if len(ln.operands) != 1:
                        raise self.err(ln, ".org needs 1 operand")
                    new_addr = self._eval_constant(ln, ln.operands[0])
                    if new_addr < addr:
                        raise self.err(ln, f".org goes backwards: {new_addr:#x} < {addr:#x}")
                    addr = new_addr
                    ln.addr = addr
                    ln.size = 0
                    continue

                if ln.op == ".equ":
                    if len(ln.operands) != 2:
                        raise self.err(ln, ".equ needs NAME, value")
                    name = ln.operands[0]
                    if not _IDENT_RE.match(name):
                        raise self.err(ln, f"bad .equ name: {name}")
                    val = self._eval_constant(ln, ln.operands[1])
                    self.equs[name] = val
                    ln.size = 0
                    continue

                if ln.op == ".db":
                    n = self._size_db(ln, 1)
                    ln.size = n
                elif ln.op == ".dw":
                    n = self._size_db(ln, 2)
                    ln.size = n
                elif ln.op == ".ascii":
                    if len(ln.operands) != 1 or not (ln.operands[0].startswith('"') and ln.operands[0].endswith('"')):
                        raise self.err(ln, '.ascii needs a "string"')
                    ln.size = len(ln.operands[0]) - 2  # strip quotes
                else:
                    # Almost all CHIP-8 ops are 2 bytes. The XO-CHIP long-load
                    # (LD I, long ...) is the sole 4-byte instruction; it's
                    # registered in OP_SIZES so pass1 lays out addresses right.
                    ln.size = OP_SIZES.get(ln.op.upper(), 2)
            except AsmError:
                raise
            except Exception as e:
                raise self.err(ln, str(e))

            addr += ln.size
            # XO-CHIP widened the address space to 64KB; classic ROMs simply
            # never approach the old 4KB line.
            if addr > 0x10000:
                raise self.err(ln, f"address overflow ({addr:#x} > 0x10000)")

    def _size_db(self, ln: Line, unit: int) -> int:
        if not ln.operands:
            raise self.err(ln, f"{ln.op} needs at least one value")
        return unit * len(ln.operands)

    def _eval_constant(self, ln: Line, tok: str) -> int:
        try:
            return parse_number(tok, self.equs)
        except ValueError as e:
            raise self.err(ln, str(e))

    # ---- pass 2: emit ----
    def pass2(self) -> bytes:
        out = bytearray()
        # We assemble to a contiguous buffer starting at start_address, and
        # honor .org by zero-padding gaps.
        for ln in self.lines:
            if ln.op is None or ln.size == 0:
                continue
            # Pad up to ln.addr.
            target_offset = ln.addr - self.start_address
            if target_offset < len(out):
                raise self.err(ln, "internal: address moved backwards")
            if target_offset > len(out):
                out.extend(b"\x00" * (target_offset - len(out)))

            try:
                ln.bytes_ = self._encode(ln)
            except AsmError:
                raise
            except Exception as e:
                raise self.err(ln, str(e))

            if len(ln.bytes_) != ln.size:
                raise self.err(ln, f"internal: size mismatch ({ln.size} vs {len(ln.bytes_)})")
            out.extend(ln.bytes_)
        return bytes(out)

    # ---- operand resolution ----
    def _resolve(self, ln: Line, tok: str) -> int:
        tok = tok.strip()
        if tok in self.symbols:
            return self.symbols[tok]
        if tok in self.equs:
            return self.equs[tok]
        try:
            return parse_number(tok, self.equs)
        except ValueError:
            raise self.err(ln, f"unknown symbol or bad number: {tok!r}")

    # ---- encoders ----
    def _encode(self, ln: Line) -> bytes:
        op = ln.op
        ops = ln.operands
        try:
            if op == ".db":
                vals = [chk_byte(".db", self._resolve(ln, t)) for t in ops]
                return bytes(vals)
            if op == ".dw":
                out = bytearray()
                for t in ops:
                    w = self._resolve(ln, t) & 0xFFFF
                    out.append((w >> 8) & 0xFF)
                    out.append(w & 0xFF)
                return bytes(out)
            if op == ".ascii":
                s = ops[0][1:-1]
                # support \n \t \\ \"
                s = (s.replace("\\n", "\n").replace("\\t", "\t")
                       .replace('\\"', '"').replace("\\\\", "\\"))
                return s.encode("latin-1")

            # 2-byte CHIP-8 instructions
            handler = ENCODERS.get(op)
            if handler is None:
                raise self.err(ln, f"unknown mnemonic: {op}")
            return handler(self, ln, ops)
        except AsmError:
            raise
        except Exception as e:
            raise self.err(ln, str(e))

    # ---- public ----
    def assemble(self) -> tuple[bytes, list[Line]]:
        self.pass1()
        rom = self.pass2()
        return rom, self.lines


# --------- per-mnemonic encoders ---------

def _need(n: int, ops: list[str], op: str) -> None:
    if len(ops) != n:
        raise AsmError(f"{op}: expected {n} operand(s), got {len(ops)}")


def enc_CLS(a, ln, ops):  _need(0, ops, "CLS");  return emit(0x00, 0xE0)
def enc_RET(a, ln, ops):  _need(0, ops, "RET");  return emit(0x00, 0xEE)
def enc_EXIT(a, ln, ops): _need(0, ops, "EXIT"); return emit(0x00, 0xFD)
def enc_LOW(a, ln, ops):  _need(0, ops, "LOW");  return emit(0x00, 0xFE)
def enc_HIGH(a, ln, ops): _need(0, ops, "HIGH"); return emit(0x00, 0xFF)
def enc_SCR(a, ln, ops):  _need(0, ops, "SCR");  return emit(0x00, 0xFB)
def enc_SCL(a, ln, ops):  _need(0, ops, "SCL");  return emit(0x00, 0xFC)


def enc_SCD(a, ln, ops):
    _need(1, ops, "SCD")
    n = chk_nibble("N", a._resolve(ln, ops[0]))
    return emit(0x00, 0xC0 | n)


def enc_SCU(a, ln, ops):
    _need(1, ops, "SCU")
    n = chk_nibble("N", a._resolve(ln, ops[0]))
    return emit(0x00, 0xD0 | n)


def enc_JP(a, ln, ops):
    # JP addr   |   JP V0, addr   (BNNN)
    if len(ops) == 1:
        addr = chk_addr("addr", a._resolve(ln, ops[0]))
        return emit(0x10 | ((addr >> 8) & 0xF), addr & 0xFF)
    if len(ops) == 2:
        if ops[0].strip().upper() != "V0":
            raise AsmError("JP <reg>, addr: reg must be V0")
        addr = chk_addr("addr", a._resolve(ln, ops[1]))
        return emit(0xB0 | ((addr >> 8) & 0xF), addr & 0xFF)
    raise AsmError("JP: bad operands")


def enc_CALL(a, ln, ops):
    _need(1, ops, "CALL")
    addr = chk_addr("addr", a._resolve(ln, ops[0]))
    return emit(0x20 | ((addr >> 8) & 0xF), addr & 0xFF)


def enc_SE(a, ln, ops):
    _need(2, ops, "SE")
    x = parse_vreg(ops[0])
    if is_vreg(ops[1]):
        y = parse_vreg(ops[1])
        return emit(0x50 | x, (y << 4))
    nn = chk_byte("byte", a._resolve(ln, ops[1]))
    return emit(0x30 | x, nn)


def enc_SNE(a, ln, ops):
    _need(2, ops, "SNE")
    x = parse_vreg(ops[0])
    if is_vreg(ops[1]):
        y = parse_vreg(ops[1])
        return emit(0x90 | x, (y << 4))
    nn = chk_byte("byte", a._resolve(ln, ops[1]))
    return emit(0x40 | x, nn)


def enc_LD(a, ln, ops):
    _need(2, ops, "LD")
    dst, src = ops[0].strip(), ops[1].strip()

    # Special destinations (case-insensitive).
    du = dst.upper()
    su = src.upper()

    # LD I, addr
    if du == "I":
        addr = chk_addr("addr", a._resolve(ln, src))
        return emit(0xA0 | ((addr >> 8) & 0xF), addr & 0xFF)
    # LD DT, Vx
    if du == "DT":
        x = parse_vreg(src)
        return emit(0xF0 | x, 0x15)
    # LD ST, Vx
    if du == "ST":
        x = parse_vreg(src)
        return emit(0xF0 | x, 0x18)
    # LD F, Vx
    if du == "F":
        x = parse_vreg(src)
        return emit(0xF0 | x, 0x29)
    # LD HF, Vx  (SCHIP big font)
    if du == "HF":
        x = parse_vreg(src)
        return emit(0xF0 | x, 0x30)
    # LD B, Vx
    if du == "B":
        x = parse_vreg(src)
        return emit(0xF0 | x, 0x33)
    # LD [I], Vx
    if du in ("[I]", "(I)"):
        x = parse_vreg(src)
        return emit(0xF0 | x, 0x55)
    # LD R, Vx  (SCHIP RPL store)
    if du == "R":
        x = parse_vreg(src)
        return emit(0xF0 | x, 0x75)

    # Vx targets
    if not is_vreg(dst):
        raise AsmError(f"LD: bad destination {dst!r}")
    x = parse_vreg(dst)

    # LD Vx, DT
    if su == "DT":
        return emit(0xF0 | x, 0x07)
    # LD Vx, K
    if su == "K":
        return emit(0xF0 | x, 0x0A)
    # LD Vx, [I]
    if su in ("[I]", "(I)"):
        return emit(0xF0 | x, 0x65)
    # LD Vx, R
    if su == "R":
        return emit(0xF0 | x, 0x85)
    # LD Vx, Vy
    if is_vreg(src):
        y = parse_vreg(src)
        return emit(0x80 | x, (y << 4))
    # LD Vx, byte
    nn = chk_byte("byte", a._resolve(ln, src))
    return emit(0x60 | x, nn)


def enc_ADD(a, ln, ops):
    _need(2, ops, "ADD")
    dst, src = ops[0].strip(), ops[1].strip()
    if dst.upper() == "I":
        x = parse_vreg(src)
        return emit(0xF0 | x, 0x1E)
    x = parse_vreg(dst)
    if is_vreg(src):
        y = parse_vreg(src)
        return emit(0x80 | x, (y << 4) | 0x4)
    nn = chk_byte("byte", a._resolve(ln, src))
    return emit(0x70 | x, nn)


def _xy(op_hi: int, n: int):
    def f(a, ln, ops):
        _need(2, ops, "")
        x = parse_vreg(ops[0]); y = parse_vreg(ops[1])
        return emit(op_hi | x, (y << 4) | n)
    return f


def enc_SHR(a, ln, ops):
    if not (1 <= len(ops) <= 2):
        raise AsmError("SHR: 1 or 2 operands")
    x = parse_vreg(ops[0])
    y = parse_vreg(ops[1]) if len(ops) == 2 else x
    return emit(0x80 | x, (y << 4) | 0x6)


def enc_SHL(a, ln, ops):
    if not (1 <= len(ops) <= 2):
        raise AsmError("SHL: 1 or 2 operands")
    x = parse_vreg(ops[0])
    y = parse_vreg(ops[1]) if len(ops) == 2 else x
    return emit(0x80 | x, (y << 4) | 0xE)


def enc_RND(a, ln, ops):
    _need(2, ops, "RND")
    x = parse_vreg(ops[0])
    nn = chk_byte("byte", a._resolve(ln, ops[1]))
    return emit(0xC0 | x, nn)


def enc_DRW(a, ln, ops):
    _need(3, ops, "DRW")
    x = parse_vreg(ops[0]); y = parse_vreg(ops[1])
    n = chk_nibble("N", a._resolve(ln, ops[2]))
    return emit(0xD0 | x, (y << 4) | n)


def enc_SKP(a, ln, ops):
    _need(1, ops, "SKP"); x = parse_vreg(ops[0])
    return emit(0xE0 | x, 0x9E)


def enc_SKNP(a, ln, ops):
    _need(1, ops, "SKNP"); x = parse_vreg(ops[0])
    return emit(0xE0 | x, 0xA1)


def enc_SYS(a, ln, ops):
    _need(1, ops, "SYS")
    addr = chk_addr("addr", a._resolve(ln, ops[0]))
    return emit(0x00 | ((addr >> 8) & 0xF), addr & 0xFF)


# ---- MX-8 custom extensions ----
def enc_MUL(a, ln, ops):
    _need(2, ops, "MUL"); x = parse_vreg(ops[0]); y = parse_vreg(ops[1])
    return emit(0x50 | x, (y << 4) | 0x1)

def enc_DIV(a, ln, ops):
    _need(2, ops, "DIV"); x = parse_vreg(ops[0]); y = parse_vreg(ops[1])
    return emit(0x50 | x, (y << 4) | 0x2)

def enc_SWAP(a, ln, ops):
    _need(2, ops, "SWAP"); x = parse_vreg(ops[0]); y = parse_vreg(ops[1])
    return emit(0x50 | x, (y << 4) | 0x3)

def enc_MEMCPY(a, ln, ops):
    _need(1, ops, "MEMCPY")
    n = a._resolve(ln, ops[0])
    if not (1 <= n <= 16):
        raise AsmError(f"MEMCPY count must be 1..16, got {n}")
    return emit(0xF0 | (n - 1), 0x50)

def enc_MEMSET(a, ln, ops):
    _need(1, ops, "MEMSET")
    n = a._resolve(ln, ops[0])
    if not (1 <= n <= 16):
        raise AsmError(f"MEMSET count must be 1..16, got {n}")
    return emit(0xF0 | (n - 1), 0x60)

def enc_RNDSEED(a, ln, ops):
    _need(1, ops, "RNDSEED")
    n = a._resolve(ln, ops[0])
    if not (1 <= n <= 8):
        raise AsmError(f"RNDSEED reg-count must be 1..8, got {n}")
    return emit(0xF0 | (n - 1), 0x70)


# ---- XO-CHIP extensions ----
def enc_PLANE(a, ln, ops):
    _need(1, ops, "PLANE")
    n = a._resolve(ln, ops[0])
    if not (0 <= n <= 3):
        raise AsmError(f"PLANE mask must be 0..3, got {n}")
    return emit(0xF0 | (n & 0x3), 0x01)

def enc_AUDIO(a, ln, ops):
    _need(0, ops, "AUDIO")
    return emit(0xF0, 0x02)

def enc_PITCH(a, ln, ops):
    _need(1, ops, "PITCH"); x = parse_vreg(ops[0])
    return emit(0xF0 | x, 0x3A)

def enc_SAVE(a, ln, ops):
    # SAVE Vx-Vy  (5XY2). Accept "Vx-Vy" as one token or two operands.
    x, y = _parse_reg_range(ops, "SAVE")
    return emit(0x50 | x, (y << 4) | 0x2)

def enc_LOAD(a, ln, ops):
    # LOAD Vx-Vy  (5XY3).
    x, y = _parse_reg_range(ops, "LOAD")
    return emit(0x50 | x, (y << 4) | 0x3)

def enc_LDLONG(a, ln, ops):
    # XO-CHIP long-load: F000 NNNN — a 4-byte instruction setting a full
    # 16-bit I. Spelled "LDLONG addr" (the assembler's mnemonic for the
    # i := long NNNN form). The operand may be a 16-bit label or constant.
    _need(1, ops, "LDLONG")
    addr = a._resolve(ln, ops[0]) & 0xFFFF
    return bytes([0xF0, 0x00, (addr >> 8) & 0xFF, addr & 0xFF])

def _parse_reg_range(ops, name):
    # Accepts "Vx-Vy" (one token) or "Vx", "Vy" (two operands).
    if len(ops) == 1 and "-" in ops[0]:
        lhs, rhs = ops[0].split("-", 1)
        return parse_vreg(lhs), parse_vreg(rhs)
    if len(ops) == 2:
        return parse_vreg(ops[0]), parse_vreg(ops[1])
    raise AsmError(f"{name}: expected Vx-Vy (got {ops})")


# Instructions whose encoding is not 2 bytes. pass1 consults this for layout.
OP_SIZES = {
    "LDLONG": 4,
}


ENCODERS = {
    "CLS": enc_CLS, "RET": enc_RET, "JP": enc_JP, "CALL": enc_CALL,
    "SE": enc_SE, "SNE": enc_SNE, "LD": enc_LD, "ADD": enc_ADD,
    "OR":  _xy(0x80, 0x1), "AND": _xy(0x80, 0x2), "XOR": _xy(0x80, 0x3),
    "SUB": _xy(0x80, 0x5), "SUBN": _xy(0x80, 0x7),
    "SHR": enc_SHR, "SHL": enc_SHL,
    "RND": enc_RND, "DRW": enc_DRW, "SKP": enc_SKP, "SKNP": enc_SKNP,
    "SYS": enc_SYS,
    "EXIT": enc_EXIT, "LOW": enc_LOW, "HIGH": enc_HIGH,
    "SCR": enc_SCR, "SCL": enc_SCL, "SCD": enc_SCD, "SCU": enc_SCU,
    # MX-8 custom extensions:
    "MUL": enc_MUL, "DIV": enc_DIV, "SWAP": enc_SWAP,
    "MEMCPY": enc_MEMCPY, "MEMSET": enc_MEMSET, "RNDSEED": enc_RNDSEED,
    # XO-CHIP extensions:
    "PLANE": enc_PLANE, "AUDIO": enc_AUDIO, "PITCH": enc_PITCH,
    "SAVE": enc_SAVE, "LOAD": enc_LOAD, "LDLONG": enc_LDLONG,
}


def make_listing(lines: list[Line]) -> str:
    out = []
    out.append(f"{'addr':>4}  {'bytes':<10}  source")
    for ln in lines:
        if ln.op is None and ln.label is None:
            continue
        b = ln.bytes_.hex().upper()
        if len(b) > 10: b = b[:8] + ".."
        out.append(f"{ln.addr:03X}   {b:<10}  {ln.raw}")
    return "\n".join(out)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="CHIP-8 / SUPER-CHIP assembler")
    ap.add_argument("input", help="source .asm/.s file")
    ap.add_argument("-o", "--output", help="output .ch8 ROM (default: input with .ch8 suffix)")
    ap.add_argument("--listing", action="store_true", help="print address+bytes listing to stdout")
    ap.add_argument("--symbols", action="store_true", help="print resolved symbol table")
    args = ap.parse_args(argv)

    src_path = Path(args.input)
    if not src_path.exists():
        print(f"error: file not found: {src_path}", file=sys.stderr)
        return 2

    out_path = Path(args.output) if args.output else src_path.with_suffix(".ch8")

    src = src_path.read_text(encoding="utf-8")
    asm = Assembler(src, str(src_path))
    try:
        rom, lines = asm.assemble()
    except AsmError as e:
        print(str(e), file=sys.stderr)
        return 1

    out_path.write_bytes(rom)
    print(f"wrote {len(rom)} bytes -> {out_path}")
    if args.symbols:
        print("symbols:")
        for k, v in sorted(asm.symbols.items(), key=lambda kv: kv[1]):
            print(f"  {v:03X}  {k}")
        for k, v in sorted(asm.equs.items()):
            print(f"  ={v:03X}  {k}")
    if args.listing:
        print(make_listing(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
