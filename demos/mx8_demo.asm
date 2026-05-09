; mx8_demo.asm — exercise MX-8 custom opcodes.
; Run with the emulator's MX-8 quirk enabled (F11 or --mx8 CLI).
;
; Computes 7 * 9 = 63 with MUL, then 63 / 5 = 12 r3 with DIV,
; SWAPs two regs, MEMSETs 4 bytes of pattern, MEMCPYs them.
; Final state visible in debugger:
;   V0 = 12 (quotient of 63/5)
;   VF = 3  (remainder of 63/5)
;   V2 = 0xAB (after SWAP)
;   V3 = 0x77 (after SWAP)
;   memory[I..I+3]   = 0x77 0x77 0x77 0x77   (MEMSET with V0... oh wait V0 is now 12,
;                       so they will be 0x0C — that's the test)
;   memory[I+4..I+7] = same as memory[I..I+3]   (MEMCPY)

main:
    LD V1, 7
    LD V2, 9
    MUL V1, V2          ; V1 = 7*9 lo = 63 = 0x3F, VF = 0 (no overflow)

    LD V0, V1           ; V0 = 63
    LD V2, 5
    DIV V0, V2          ; V0 = 12, VF = 3

    LD V2, 0x77
    LD V3, 0xAB
    SWAP V2, V3         ; V2 = 0xAB, V3 = 0x77

    LD I, scratch
    MEMSET 4            ; fill 4 bytes at scratch with V0 (=12 = 0x0C)
    MEMCPY 4            ; copy those 4 bytes to scratch+4

halt:
    EXIT                ; SCHIP exit

scratch:
    .db 0,0,0,0,0,0,0,0
