#include "Xo8ISA.hpp"

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

// ---- 0x0NNN: XO-CHIP scrolls are plane-aware. 00FD EXIT / 00FE LOW /
// 00FF HIGH / CLS / RET defer to the SCHIP/base implementations.
void Xo8ISA::dispatch0(Chip8& cpu, uint16_t opcode) {
    const uint8_t nn = opcode & 0x00FF;
    const uint8_t n  = opcode & 0x000F;

    if ((nn & 0xF0) == 0xC0) { cpu.scrollDownPlanes(n); return; }
    if ((nn & 0xF0) == 0xD0) { cpu.scrollUpPlanes(n);   return; }

    switch (nn) {
    case 0xFB: cpu.scrollRightPlanes(4); break;
    case 0xFC: cpu.scrollLeftPlanes(4);  break;
    default:   SchipISA::dispatch0(cpu, opcode); break;  // EXIT/LOW/HIGH/CLS/RET
    }
}

// ---- 0x5XYN: XO-CHIP claims 5XY2 (save vx-vy) and 5XY3 (load vx-vy).
// Both operate on a register *range* V_X..V_Y (either ascending or
// descending) and, unlike FX55/FX65, never modify I.
void Xo8ISA::dispatch5(Chip8& cpu, uint16_t opcode) {
    const uint8_t x = (opcode & 0x0F00) >> 8;
    const uint8_t y = (opcode & 0x00F0) >> 4;
    const uint8_t n =  opcode & 0x000F;
    auto& v = cpu.v;

    // 5XY0 = SE Vx, Vy — fall back to base.
    if (n == 0) { SchipISA::dispatch5(cpu, opcode); return; }

    if (n != 2 && n != 3) { cpu.halt(Chip8::HaltReason::UnknownOpcode); return; }

    // The range may run up or down; the number of registers is |x-y|+1.
    const int step  = (x <= y) ? 1 : -1;
    const int count = (x <= y ? (y - x) : (x - y)) + 1;

    if (cpu.index + count > Chip8::MEMORY_SIZE) {
        cpu.halt(Chip8::HaltReason::BadMemoryAccess); return;
    }

    if (n == 2) {              // save vx-vy : registers -> [I..]
        int reg = x;
        for (int i = 0; i < count; ++i, reg += step) {
            cpu.mem.write(cpu.index + i, v[reg]);
        }
    } else {                   // load vx-vy : [I..] -> registers
        int reg = x;
        for (int i = 0; i < count; ++i, reg += step) {
            v[reg] = cpu.mem.read(cpu.index + i);
        }
    }
    // I is intentionally left unchanged (XO-CHIP spec).
}

// ---- 0xFXNN plus the special 4-byte 0xF000 NNNN long-load.
void Xo8ISA::dispatchF(Chip8& cpu, uint16_t opcode) {
    const uint8_t x  = (opcode & 0x0F00) >> 8;
    const uint8_t nn =  opcode & 0x00FF;

    // F000 NNNN — 4-byte instruction. The two bytes following the opcode form
    // a full 16-bit address loaded into I. The CPU already advanced PC past
    // the F000 word; we consume the NNNN word and advance PC past it too.
    if (opcode == 0xF000) {
        if (cpu.pc + 1 >= Chip8::MEMORY_SIZE) {
            cpu.halt(Chip8::HaltReason::BadMemoryAccess); return;
        }
        uint16_t hi = cpu.mem.read(cpu.pc);
        uint16_t lo = cpu.mem.read(cpu.pc + 1);
        cpu.index = static_cast<uint16_t>((hi << 8) | lo);
        cpu.pc += 2;
        return;
    }

    switch (nn) {
    case 0x01: {  // FN01 — plane N: select draw-plane bitmask (N in 0..3).
        cpu.plane_mask = x & 0x3;
        break;
    }
    case 0x02: {  // F002 — audio: load 16-byte pattern buffer from [I].
        if (cpu.index + Chip8::AUDIO_PATTERN_BYTES > Chip8::MEMORY_SIZE) {
            cpu.halt(Chip8::HaltReason::BadMemoryAccess); break;
        }
        for (int i = 0; i < Chip8::AUDIO_PATTERN_BYTES; ++i) {
            cpu.audio_pattern[i] = cpu.mem.read(cpu.index + i);
        }
        break;
    }
    case 0x3A: {  // FX3A — pitch vx: set audio playback pitch.
        cpu.audio_pitch = cpu.v[x];
        break;
    }
    default:
        SchipISA::dispatchF(cpu, opcode);
        break;
    }
}

// ============== disassembly ==============

std::string Xo8ISA::dis0(uint16_t op) const {
    const uint8_t nn = op & 0x00FF;
    const uint8_t n  = op & 0x000F;
    if ((nn & 0xF0) == 0xC0) return fmt("SCD %X", n);
    if ((nn & 0xF0) == 0xD0) return fmt("SCU %X", n);
    if (nn == 0xFB) return "SCR";
    if (nn == 0xFC) return "SCL";
    return SchipISA::dis0(op);
}

std::string Xo8ISA::dis5(uint16_t op) const {
    const uint8_t x = (op & 0x0F00) >> 8;
    const uint8_t y = (op & 0x00F0) >> 4;
    const uint8_t n =  op & 0x000F;
    if (n == 2) return fmt("SAVE V%X-V%X", x, y);
    if (n == 3) return fmt("LOAD V%X-V%X", x, y);
    return SchipISA::dis5(op);
}

std::string Xo8ISA::disF(uint16_t op) const {
    const uint8_t x  = (op & 0x0F00) >> 8;
    const uint8_t nn =  op & 0x00FF;
    if (op == 0xF000) return "LD I, long";   // 4-byte; operand shown by debugger separately
    if (nn == 0x01) return fmt("PLANE %X", x);
    if (nn == 0x02) return "AUDIO";
    if (nn == 0x3A) return fmt("PITCH V%X", x);
    return SchipISA::disF(op);
}
