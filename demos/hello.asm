; hello.asm — draw the word "HI" centered, then bounce a pixel around.
; Demonstrates: labels, .org default 0x200, sprite data with .db,
; basic graphics (DRW), keypad polling, simple game loop.

    .equ SCR_W, 64
    .equ SCR_H, 32

main:
    CLS
    LD V0, 24            ; x for 'H'
    LD V1, 12            ; y
    LD I, glyph_H
    DRW V0, V1, 5
    LD V0, 32            ; x for 'I'
    LD I, glyph_I
    DRW V0, V1, 5

    ; bouncing pixel
    LD V2, 1             ; ball x
    LD V3, 1             ; ball y
    LD V4, 1             ; dx
    LD V5, 1             ; dy

loop:
    LD I, ball
    DRW V2, V3, 1        ; erase
    ADD V2, V4
    ADD V3, V5

    ; bounce off right edge
    LD V6, 63
    SE V2, V6
    JP no_right
    LD V4, 0xFF          ; -1
no_right:
    SNE V2, 0
    LD V4, 1

    LD V6, 31
    SE V3, V6
    JP no_down
    LD V5, 0xFF
no_down:
    SNE V3, 0
    LD V5, 1

    LD I, ball
    DRW V2, V3, 1        ; redraw

    ; small delay so it's watchable
    LD V7, 2
    LD DT, V7
wait:
    LD V8, DT
    SE V8, 0
    JP wait

    JP loop

; --- data ---
ball:
    .db 0x80
glyph_H:
    .db 0x90,0x90,0xF0,0x90,0x90
glyph_I:
    .db 0xE0,0x40,0x40,0x40,0xE0
