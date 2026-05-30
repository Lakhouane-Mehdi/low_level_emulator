# ROM credits

Bundled ROMs and their provenance.

## Public-domain classics
`IBM_Logo.ch8`, `PONG.ch8`, `BREAKOUT.ch8`, `TETRIS.ch8` — classic
public-domain CHIP-8 programs that have circulated freely for decades.

## Assembled demos (this repo)
`hello.ch8`, `maze.ch8`, `bounce.ch8`, `mx8_demo.ch8`, `xo_demo.ch8` —
assembled from the sources in [`demos/`](../demos) with `tools/asm.py`.
Authored for this project.

## chip8Archive ROMs (CC0)
The following are from [JohnEarnest/chip8Archive](https://github.com/JohnEarnest/chip8Archive),
released under CC0 (public domain dedication). Used here as accuracy
fixtures and golden-hash regression targets in `tools/test_headless.py`.

### SUPER-CHIP
| File | Archive slug | Title |
|------|--------------|-------|
| `SNAKE_SCHIP.ch8`    | `snake`    | Snake |
| `OCTOPEG_SCHIP.ch8`  | `octopeg`  | Octopeg |
| `MONDRIAN_SCHIP.ch8` | `mondrian` | Mondrian |
| `EATY_SCHIP.ch8`     | `eaty`     | Eaty the Alien |

### XO-CHIP
| File | Archive slug | Title | Notes |
|------|--------------|-------|-------|
| `SKYWARD_XO.ch8`     | `skyward`  | Skyward      | 55KB — exercises >4KB addressing |
| `JUB8_SONG1_XO.ch8`  | `jub8-1`   | jub8 Song 1  | 56KB — XO-CHIP audio |
| `T8NKS_XO.ch8`       | `t8nks`    | T8NKS        | 33KB |
| `OCTOMA_XO.ch8`      | `octoma`   | Octoma       | |
| `FLUTTERBY_XO.ch8`   | `flutterby`| Flutter By   | uses RNG — seeded in the test suite |

The Timendus CHIP-8 test suite under `roms/tests/` is from
[Timendus/chip8-test-suite](https://github.com/Timendus/chip8-test-suite)
(MIT). See that repository for its license terms.
