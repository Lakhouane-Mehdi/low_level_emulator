; bounce.asm — single ball bouncing with sub-pixel velocity tracking.
; Plain CHIP-8, no extensions. Showcases timer-driven animation
; and sprite double-buffer (XOR erase + redraw).

main:
    CLS
    LD V0, 8           ; ball x (pixels)
    LD V1, 12          ; ball y
    LD V2, 1           ; dx
    LD V3, 1           ; dy

draw_loop:
    LD I, ball
    DRW V0, V1, 4      ; XOR draw

    ; frame delay
    LD V4, 2
    LD DT, V4
wait:
    LD V4, DT
    SE V4, 0
    JP wait

    LD I, ball
    DRW V0, V1, 4      ; XOR erase

    ADD V0, V2
    ADD V1, V3

    ; right wall: x == 60 (4-wide ball)
    LD V5, 60
    SNE V0, V5
    LD V2, 0xFF        ; -1
    ; left wall: x == 0
    SNE V0, 0
    LD V2, 1
    ; bottom: y == 28
    LD V5, 28
    SNE V1, V5
    LD V3, 0xFF
    ; top: y == 0
    SNE V1, 0
    LD V3, 1

    JP draw_loop

ball:
    .db 0x60, 0xF0, 0xF0, 0x60
