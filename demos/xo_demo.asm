; xo_demo.asm — XO-CHIP feature demo.
;
; Draws two overlapping 8x8 sprites, one on each bit-plane, so the overlap
; region renders in the third (combined) color. Also exercises the F000
; long-load and the plane-select opcode. Deterministic: no input, no RNG,
; halts with EXIT — perfect for a golden-hash regression fixture.
;
; Assemble:  python tools/asm.py demos/xo_demo.asm -o roms/xo_demo.ch8
; Run:       build/Release/chip8.exe roms/xo_demo.ch8 --xochip

main:
    CLS

    ; ---- plane 0: a filled square at (10, 8) ----
    PLANE 1
    LD V0, 10
    LD V1, 8
    LD I, square
    DRW V0, V1, 8

    ; ---- plane 1: the same square shifted +4,+4, so it overlaps ----
    PLANE 2
    LD V0, 14
    LD V1, 12
    LD I, square
    DRW V0, V1, 8

    ; ---- prove the 16-bit long-load works: point I at a high label and
    ;      copy a marker byte into V2 with LD Vx,[I]. (data lives past 0x300
    ;      via .org so the address is unambiguous.)
    LDLONG marker
    LD V2, [I]            ; V2 = 0xA5

done:
    EXIT

; 8x8 solid-ish square sprite (a hollow box, visually distinct on overlap).
square:
    .db 0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF

    .org 0x300
marker:
    .db 0xA5
