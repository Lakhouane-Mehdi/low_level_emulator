; maze.asm — the classic random-maze demo, but with named subroutines.
; Picks 1 of 2 diagonals randomly per cell and tiles the screen.
; Demonstrates: subroutines (CALL/RET), RND, nested loops, .equ.

    .equ COLS, 16        ; 64 / 4
    .equ ROWS, 8         ; 32 / 4

main:
    CLS
    LD V0, 0             ; x cursor
    LD V1, 0             ; y cursor
row_loop:
    LD V2, 0             ; x = 0
inner:
    CALL draw_tile
    ADD V2, 4
    LD V3, 64
    SNE V2, V3
    JP next_row
    JP inner
next_row:
    ADD V1, 4
    LD V3, 32
    SNE V1, V3
    JP forever
    JP row_loop
forever:
    JP forever

; Pick one of two diagonals, draw it at (V2, V1).
draw_tile:
    RND V4, 1             ; random 0 or 1
    SE V4, 0
    JP slash
    LD I, backslash
    JP draw_it
slash:
    LD I, fwdslash
draw_it:
    DRW V2, V1, 4
    RET

; 4-row sprites: forward slash and back slash.
fwdslash:
    .db 0x10, 0x20, 0x40, 0x80
backslash:
    .db 0x80, 0x40, 0x20, 0x10
