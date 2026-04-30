# CHIP-8 / SUPER-CHIP Emulator

A functional CHIP-8 + SUPER-CHIP emulator in C++17 with SFML 3, featuring a
real-time debugger sidebar, ROM picker, save states, and rewind.

![CHIP-8 emulator running IBM Logo with the live debugger sidebar](screenshots/debugger.png)

## Features

- All 35 standard CHIP-8 opcodes + the full SUPER-CHIP 1.1 extension set
  (128x64 hi-res mode, 16x16 sprites, scroll, big font, RPL flags, exit)
- 64x32 lo-res or 128x64 hi-res, scaled to a 768x384 game window
- 60Hz timers, 600Hz CPU, square-wave 440Hz beep on the sound timer
- 16-key hex keypad mapped to the keyboard
- **Live debugger sidebar**: registers V0-VF, PC, I, SP, timers, next 5
  disassembled instructions, full stack, and a 32-byte memory window
  centered on the index register I
- **Pause + single-step** execution
- **Save state + rewind**: F5 saves, F9 loads, hold Backspace to rewind up
  to 5 seconds of history
- **ROM picker**: launch with no arguments to get a clickable menu of every
  `.ch8` file in the `roms/` folder
- **Quirks toggle**: switch between modern (SCHIP) and legacy (COSMAC VIP)
  behavior for the ambiguous shift and load/store opcodes

## Build (vcpkg + Visual Studio 2022, Windows)

```bash
vcpkg install sfml:x64-windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
```

The post-build step copies `roms/` next to `chip8.exe` and vcpkg auto-deploys
all required SFML DLLs.

## Run

```bash
# launch the ROM picker
./build/Release/chip8.exe

# or load a specific ROM
./build/Release/chip8.exe roms/PONG.ch8
```

Bundled ROMs are public-domain. Many more at
https://github.com/kripod/chip8-roms

## Controls

### Game keypad

```
CHIP-8           Keyboard
1 2 3 C          1 2 3 4
4 5 6 D    -->   Q W E R
7 8 9 E          A S D F
A 0 B F          Z X C V
```

### Emulator hotkeys

| Key | Action |
|---|---|
| `Esc` | Quit |
| `Space` | Pause / resume |
| `N` | Single-step one instruction (while paused) |
| `F1` | Toggle quirks: modern vs. legacy COSMAC VIP behavior |
| `F5` | Save state |
| `F9` | Load saved state |
| `Backspace` | Hold to rewind (5 seconds of history) |

### Debugger sidebar

Always visible on the right of the window:

- Top: emulator status, hotkey reminders, current quirks/resolution mode
- PC, I, SP, DT (delay), ST (sound) timers
- All 16 V-registers in a 4x4 grid
- Next 5 disassembled instructions, current PC marked `>`
- Full stack with `>` next to current SP
- 32-byte memory window centered on the index register I, with the byte at
  I bracketed `[..]` so you can watch sprite data being read by `DXYN`

## Project layout

```
src/
  Chip8.hpp / .cpp        CPU, memory, opcodes, snapshots
  Disassembler.hpp / .cpp Opcode -> mnemonic
  main.cpp                SFML window, audio, input, debugger sidebar
roms/                     Public-domain test ROMs
CMakeLists.txt
```

## Author

Made by Mehdi Lakhouane
