# CHIP-8 Emulator

A functional CHIP-8 emulator in C++17 with SFML 3.x.

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
./build/Release/chip8.exe roms/IBM_Logo.ch8
./build/Release/chip8.exe roms/PONG.ch8
./build/Release/chip8.exe roms/TETRIS.ch8
./build/Release/chip8.exe roms/BREAKOUT.ch8
```

Bundled ROMs are public-domain. More at https://github.com/kripod/chip8-roms

## Keypad

```
CHIP-8           Keyboard
1 2 3 C          1 2 3 4
4 5 6 D    -->   Q W E R
7 8 9 E          A S D F
A 0 B F          Z X C V
```

`Esc` quits.

## Author

Made by Mehdi Lakhouane
