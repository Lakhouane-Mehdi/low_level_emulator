# CHIP-8 / SUPER-CHIP / MX-8 / XO-CHIP Emulator

A C++17 emulator, interactive debugger, and Python assembler for CHIP-8,
SUPER-CHIP 1.1, **MX-8** (six opcodes of my own layered on the classic ISA),
and **XO-CHIP** (Octo's 64KB / two-bit-plane / 4-color extension).

![CHIP-8 emulator running IBM Logo with the live debugger sidebar](screenshots/debugger.png)

## What's in the box

- **Emulator core** in `src/core/` — no graphics, no I/O coupling. 35 CHIP-8
  opcodes, the full SUPER-CHIP 1.1 set, 6 MX-8 extensions, and XO-CHIP.
- **Debugger sidebar** — registers, disassembly with breakpoint markers, stack,
  three memory panes, instruction trace, status line.
- **Real debugger controls** — pause, step, step over `CALL`, step out,
  breakpoints, watchpoints, a mid-execution memory editor.
- **Save state and rewind** on a ring buffer.
- **Seven quirks toggled independently** (F1–F7), or one-shot to MODERN (F8) or
  VIP (F12).
- **ROM browser**, seven palettes, configurable speed 1–2000 cycles/frame.
- **Python assembler** at `tools/asm.py` with a 54-case self-test.

## Build

Windows, vcpkg, Visual Studio 2022:

```
vcpkg install sfml:x64-windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
```

Linux and macOS use the same CMakeLists once you point CMake at an SFML 3
install.

## Run

```
./build/Release/chip8.exe                       # ROM picker
./build/Release/chip8.exe roms/PONG.ch8
./build/Release/chip8.exe roms/TETRIS.ch8 --speed 25 --palette amber --legacy
./build/Release/chip8.exe roms/mx8_demo.ch8 --mx8 --paused
```

| Flag | Meaning |
|---|---|
| `--speed N` | CPU cycles per frame (1..2000, default 12) |
| `--legacy` | Boot with COSMAC VIP quirks |
| `--mx8` | Enable MX-8 opcodes from start |
| `--xochip`, `--xo` | Run in XO-CHIP mode |
| `--paused` | Boot paused |
| `--palette NAME` | mono / amber / green / gameboy / c64 / ice / hotdog |
| `--rewind-sec N` | Seconds of rewind history (default 5) |
| `--roms-dir DIR` | Where the picker scans (default `roms`) |

## Keys

```
CHIP-8           Keyboard
1 2 3 C          1 2 3 4
4 5 6 D    -->   Q W E R
7 8 9 E          A S D F
A 0 B F          Z X C V
```

`Space` pause, `N` step, `O` step over a `CALL`, `Enter` step out, `B`
breakpoint at PC, `W` cycle watchpoint at cursor. `F1`–`F7` toggle individual
quirks, `F8` MODERN preset, `F12` LEGACY, `F11` MX-8, `Shift+F11` XO-CHIP.
`F5`/`F9` save and load state, hold `Backspace` to rewind, `+`/`-` speed,
`F10` palette, `M` cycle memory pane, `[`/`]` edit byte at cursor.

Full list in the pause menu.

## MX-8

Six opcodes of my own, on `F11` or `--mx8`, encoded into unused slots so they
can't collide with classic ROMs.

| Opcode | Mnemonic | Effect |
|--------|----------|--------|
| `5XY1` | `MUL Vx, Vy` | `Vx = (Vx * Vy) & 0xFF`, `VF` = overflow byte |
| `5XY2` | `DIV Vx, Vy` | `Vx = Vx / Vy`, `VF` = remainder; `VF = 0xFF` if `Vy == 0` |
| `5XY3` | `SWAP Vx, Vy` | Exchange in one cycle |
| `FX50` | `MEMCPY n` | Copy `n = X+1` bytes from `[I]` to `[I + n]` |
| `FX60` | `MEMSET n` | Fill `n = X+1` bytes at `[I]` with `V0` |
| `FX70` | `RNDSEED n` | Seed the PRNG from `V0..V(n-1)` |

These plug holes the original ISA leaves — there's no native multiply, no fast
block fill or copy, and no way to make the RNG deterministic for testing.

## XO-CHIP

The de-facto modern CHIP-8 extension, popularized by Octo. Run with `--xochip`
or toggle at runtime with `Shift+F11`.

It's a **sibling** of MX-8, not a superset — both claim `5XY2`/`5XY3` with
incompatible meanings, so a machine runs one or the other.

| Opcode | Mnemonic | Effect |
|--------|----------|--------|
| `FN01` | `PLANE n` | Select draw-plane bitmask (0..3) |
| `5XY2` | `SAVE Vx-Vy` | Store register range to `[I]`, I unchanged |
| `5XY3` | `LOAD Vx-Vy` | Load register range from `[I]`, I unchanged |
| `F000 NNNN` | `LDLONG addr` | 4-byte op, loads a 16-bit address into I |
| `F002` | `AUDIO` | Load the 16-byte audio pattern buffer from `[I]` |
| `FX3A` | `PITCH Vx` | Playback pitch, `4000·2^((pitch-64)/48)` Hz |
| `00CN/00DN` | `SCD/SCU n` | Scroll down/up, plane-aware |
| `00FB/00FC` | `SCR/SCL` | Scroll 4px right/left, plane-aware |

Concretely that's **64KB** of address space instead of 4KB, **two bit-planes
giving four colors**, and **real audio** synthesized from the pattern buffer
rather than a fixed 440Hz beep.

Classic ROMs never touch beyond 4KB and only ever light plane 0, so they render
byte-identically to before — the golden framebuffer hashes don't move.

See [src/core/isa/Xo8ISA.cpp](src/core/isa/Xo8ISA.cpp) and the `test_xo_*` cases
in [tests/core_test.cpp](tests/core_test.cpp). A worked example is in
[demos/xo_demo.asm](demos/xo_demo.asm).

## Assembler

```
python tools/asm.py demos/hello.asm -o demos/hello.ch8
python tools/asm.py demos/maze.asm --listing --symbols
python tools/test_asm.py                  # opcode self-test
```

```asm
    .equ HIGH_X, 0x3C
main:
    CLS
    LD V0, HIGH_X
    LD I, sprite
    DRW V0, V1, 5
loop:
    JP loop
sprite:
    .db 0xF0, 0x90, 0x90, 0x90, 0xF0
```

Numbers as `12`, `0x12`, `$12`, `0b1010`, `%1010`. Labels case-sensitive,
mnemonics not. Working examples in `demos/*.asm`.

## Event-driven state mutation

Every UI, debugger, and scripted state change goes through a typed event queue
on the CPU, drained at the frame boundary.

```cpp
cpu.enqueue(WriteMemoryEvent{0x300, 0xAB});
cpu.enqueue(ToggleBreakpointEvent{0x250});
cpu.enqueue(InjectKeyEvent{0xA, true});
cpu.enqueue(SetSeedEvent{0xCAFEBABE});
```

Events are pure data (`std::variant`) — no lambdas, no captured state — so
they're serializable and replayable. Only machine-state mutations are events;
configuration and UI mode stay direct.

This is what makes the next three sections possible. Full type list in
`src/core/CoreEvents.hpp`.

## Rewind

Hold `Backspace` to step back a frame at a time.

The buffer doesn't keep a snapshot per frame. It keeps **anchors** — full
snapshots once a second — plus the event log between them. To reach frame N it
restores the latest anchor ≤ N and re-executes forward, replaying the events
originally submitted on each frame.

Memory cost is per second of window, not per frame: 30 seconds is about 180KB,
and `--rewind-sec N` can raise that to minutes without any architectural change.

Reconstruction contract in `src/core/RewindBuffer.hpp`.

## Replay

Replay files are JSON and hermetic — ROM bytes, ISA, quirks, seed, and the full
event stream.

```
# Shift+R toggles recording; writes replays/<rom>-<ts>.json
build/Release/chip8_run.exe --replay replays/breakout_demo.json --print-hash
```

The replay is the source of truth. Tamper with the ROM, seed, ISA, or quirks
and the embedded checkpoints mismatch. Sharing one lets someone reproduce your
session byte-identically without your machine's PRNG seed.

## Headless runner and diffing

`chip8_run` is a no-SFML CLI for CI and regression testing.

```
build/Release/chip8_run.exe rom.ch8 --frames 600 --print-hash
build/Release/chip8_run.exe rom.ch8 --frames 600 --expect-hash 0xFA6CE8F707AC1F88
python tools/test_headless.py             # golden hash suite
python tools/test_headless.py --update    # regenerate
```

Exit codes: 0 success, 1 hash mismatch, 2 halted early, 3 CLI or load error.

When two replays should match and don't, `--diff-replay` binary-searches the
first frame where machine state diverges and reports which component drifted:

```
build/Release/chip8_run.exe --diff-replay a.json b.json
# input streams: 2 events, identical
# searching divergence in frames [0, 600) ...
#   divergence at frame 142:
#     memory       differs: 0xBD4E9B37638590EE  vs  0x3C26171756D63C45
```

It compares input streams first and refuses to proceed if those differ —
identical inputs are a prerequisite for the execution diff to mean anything.
`--force` overrides.

`--self-check` re-runs a replay against its own checkpoints, which catches
implementation drift between builds.

## Layout

```
src/
  core/      Chip8, Memory, CoreEvents, Disassembler   # pure, no SFML
  core/isa/  IInstructionSet, Chip8ISA, SchipISA, Mx8ISA, Xo8ISA
  ui/        Renderer, Input, AudioBeep, DebugView, RomBrowser, FontLoader
  app/       Config, App
tools/       asm.py, test_asm.py, test_headless.py
demos/       .asm sources
roms/        public-domain games, assembled demos, tests/
```

## Accuracy

`roms/tests/` ships Timendus's full eight-stage CHIP-8 test suite. Drop your own
in there and pick from the browser.

For XO-CHIP, five CC0 ROMs from JohnEarnest/chip8Archive live under
`roms/*_XO.ch8` — two of them are 55–56KB and only load because of the 64KB
address space. Provenance in [roms/CREDITS.md](roms/CREDITS.md).

Made by Mehdi Lakhouane.
