#include "SchipISA.hpp"

#include "../Chip8.hpp"

#include <cstdarg>
#include <cstdio>

namespace {
std::string fmt(const char* f, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}
} // namespace

void SchipISA::dispatch0(Chip8& cpu, uint16_t opcode) {
    const uint8_t nn = opcode & 0x00FF;
    const uint8_t n  = opcode & 0x000F;

    if ((nn & 0xF0) == 0xC0) { cpu.scrollDown(n); return; }
    if ((nn & 0xF0) == 0xD0) { cpu.scrollUp(n);   return; }

    switch (nn) {
    case 0xFB: cpu.scrollRight(); break;
    case 0xFC: cpu.scrollLeft();  break;
    case 0xFD: cpu.halt(Chip8::HaltReason::ExitOpcode); break;
    case 0xFE: cpu.hires = false; cpu.clearScreen(); break;
    case 0xFF: cpu.hires = true;  cpu.clearScreen(); break;
    default:   Chip8ISA::dispatch0(cpu, opcode); break;   // CLS / RET fall-through
    }
}

void SchipISA::dispatchF(Chip8& cpu, uint16_t opcode) {
    const uint8_t x  = (opcode & 0x0F00) >> 8;
    const uint8_t nn =  opcode & 0x00FF;
    auto& v = cpu.v;

    switch (nn) {
    case 0x30:  // LD HF, Vx — big font (10 bytes per digit)
        cpu.index = Chip8::BIG_FONT_ADDRESS + (v[x] & 0xF) * 10;
        break;
    case 0x75:  // RPL store: V0..Vx -> rpl_flags
        for (int i = 0; i <= x && i < 8; ++i) cpu.rpl_flags[i] = v[i];
        break;
    case 0x85:  // RPL load:  rpl_flags -> V0..Vx
        for (int i = 0; i <= x && i < 8; ++i) v[i] = cpu.rpl_flags[i];
        break;
    default:
        Chip8ISA::dispatchF(cpu, opcode);
        break;
    }
}

std::string SchipISA::dis0(uint16_t op) const {
    const uint8_t nn = op & 0x00FF;
    const uint8_t n  = op & 0x000F;
    if ((nn & 0xF0) == 0xC0) return fmt("SCD %X", n);
    if ((nn & 0xF0) == 0xD0) return fmt("SCU %X", n);
    if (nn == 0xFB) return "SCR";
    if (nn == 0xFC) return "SCL";
    if (nn == 0xFD) return "EXIT";
    if (nn == 0xFE) return "LOW";
    if (nn == 0xFF) return "HIGH";
    return Chip8ISA::dis0(op);
}

std::string SchipISA::disF(uint16_t op) const {
    const uint8_t x  = (op & 0x0F00) >> 8;
    const uint8_t nn =  op & 0x00FF;
    if (nn == 0x30) return fmt("LD HF, V%X", x);
    if (nn == 0x75) return fmt("LD R, V%X",  x);
    if (nn == 0x85) return fmt("LD V%X, R",  x);
    return Chip8ISA::disF(op);
}
