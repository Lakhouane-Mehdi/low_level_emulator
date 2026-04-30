#include "Disassembler.hpp"

#include <cstdarg>
#include <cstdio>

static std::string fmt(const char* f, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string disassemble(uint16_t op) {
    uint16_t nnn = op & 0x0FFF;
    uint8_t  nn  = op & 0x00FF;
    uint8_t  n   = op & 0x000F;
    uint8_t  x   = (op & 0x0F00) >> 8;
    uint8_t  y   = (op & 0x00F0) >> 4;

    switch (op & 0xF000) {
    case 0x0000:
        if (nn == 0xE0) return "CLS";
        if (nn == 0xEE) return "RET";
        if ((nn & 0xF0) == 0xC0) return fmt("SCD %X", n);
        if (nn == 0xFB) return "SCR";
        if (nn == 0xFC) return "SCL";
        if (nn == 0xFD) return "EXIT";
        if (nn == 0xFE) return "LOW";
        if (nn == 0xFF) return "HIGH";
        return fmt("SYS %03X", nnn);
    case 0x1000: return fmt("JP %03X", nnn);
    case 0x2000: return fmt("CALL %03X", nnn);
    case 0x3000: return fmt("SE V%X, %02X", x, nn);
    case 0x4000: return fmt("SNE V%X, %02X", x, nn);
    case 0x5000: return fmt("SE V%X, V%X", x, y);
    case 0x6000: return fmt("LD V%X, %02X", x, nn);
    case 0x7000: return fmt("ADD V%X, %02X", x, nn);
    case 0x8000:
        switch (n) {
        case 0x0: return fmt("LD V%X, V%X", x, y);
        case 0x1: return fmt("OR V%X, V%X", x, y);
        case 0x2: return fmt("AND V%X, V%X", x, y);
        case 0x3: return fmt("XOR V%X, V%X", x, y);
        case 0x4: return fmt("ADD V%X, V%X", x, y);
        case 0x5: return fmt("SUB V%X, V%X", x, y);
        case 0x6: return fmt("SHR V%X", x);
        case 0x7: return fmt("SUBN V%X, V%X", x, y);
        case 0xE: return fmt("SHL V%X", x);
        }
        break;
    case 0x9000: return fmt("SNE V%X, V%X", x, y);
    case 0xA000: return fmt("LD I, %03X", nnn);
    case 0xB000: return fmt("JP V0, %03X", nnn);
    case 0xC000: return fmt("RND V%X, %02X", x, nn);
    case 0xD000: return fmt("DRW V%X, V%X, %X", x, y, n);
    case 0xE000:
        if (nn == 0x9E) return fmt("SKP V%X", x);
        if (nn == 0xA1) return fmt("SKNP V%X", x);
        break;
    case 0xF000:
        switch (nn) {
        case 0x07: return fmt("LD V%X, DT", x);
        case 0x0A: return fmt("LD V%X, K", x);
        case 0x15: return fmt("LD DT, V%X", x);
        case 0x18: return fmt("LD ST, V%X", x);
        case 0x1E: return fmt("ADD I, V%X", x);
        case 0x29: return fmt("LD F, V%X", x);
        case 0x30: return fmt("LD HF, V%X", x);
        case 0x33: return fmt("LD B, V%X", x);
        case 0x55: return fmt("LD [I], V%X", x);
        case 0x65: return fmt("LD V%X, [I]", x);
        case 0x75: return fmt("LD R, V%X", x);
        case 0x85: return fmt("LD V%X, R", x);
        }
        break;
    }
    return fmt("??? %04X", op);
}
