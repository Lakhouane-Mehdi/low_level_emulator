#include "Mx8ISA.hpp"

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

void Mx8ISA::dispatch5(Chip8& cpu, uint16_t opcode) {
    const uint8_t x = (opcode & 0x0F00) >> 8;
    const uint8_t y = (opcode & 0x00F0) >> 4;
    const uint8_t n =  opcode & 0x000F;

    // 5XY0 = SE Vx, Vy — fall back to base.
    if (n == 0) { SchipISA::dispatch5(cpu, opcode); return; }

    if (!cpu.quirks.mx8_extensions) {
        cpu.halt(Chip8::HaltReason::UnknownOpcode);
        return;
    }

    auto& v = cpu.v;
    switch (n) {
    case 0x1: { // MUL Vx, Vy: Vx = lo byte, VF = hi byte
        uint16_t prod = static_cast<uint16_t>(v[x]) * static_cast<uint16_t>(v[y]);
        uint8_t hi = (prod >> 8) & 0xFF;
        v[x] = prod & 0xFF;
        v[0xF] = hi;
        break;
    }
    case 0x2: { // DIV Vx, Vy: Vx = quotient, VF = remainder; VF=0xFF on /0
        if (v[y] == 0) { v[0xF] = 0xFF; }
        else {
            uint8_t q = v[x] / v[y];
            uint8_t r = v[x] % v[y];
            v[x]  = q;
            v[0xF] = r;
        }
        break;
    }
    case 0x3: { // SWAP Vx, Vy
        uint8_t tmp = v[x]; v[x] = v[y]; v[y] = tmp;
        break;
    }
    default: cpu.halt(Chip8::HaltReason::UnknownOpcode); break;
    }
}

void Mx8ISA::dispatchF(Chip8& cpu, uint16_t opcode) {
    const uint8_t x  = (opcode & 0x0F00) >> 8;
    const uint8_t nn =  opcode & 0x00FF;

    switch (nn) {
    case 0x50: { // MEMCPY (X+1) bytes from [I] to [I + X+1]
        if (!cpu.quirks.mx8_extensions) { cpu.halt(Chip8::HaltReason::UnknownOpcode); break; }
        int n = x + 1;
        if (cpu.index + 2 * n > Chip8::MEMORY_SIZE) {
            cpu.halt(Chip8::HaltReason::BadMemoryAccess); break;
        }
        // Buffer the read so a watchpoint on the destination can't see
        // partial state mid-copy.
        uint8_t buf[16];
        cpu.mem.read_block(cpu.index, buf, n);
        cpu.mem.write_block(cpu.index + n, buf, n);
        break;
    }
    case 0x60: { // MEMSET (X+1) bytes at [I] to V0
        if (!cpu.quirks.mx8_extensions) { cpu.halt(Chip8::HaltReason::UnknownOpcode); break; }
        int n = x + 1;
        if (cpu.index + n > Chip8::MEMORY_SIZE) {
            cpu.halt(Chip8::HaltReason::BadMemoryAccess); break;
        }
        uint8_t buf[16];
        for (int i = 0; i < n; ++i) buf[i] = cpu.v[0];
        cpu.mem.write_block(cpu.index, buf, n);
        break;
    }
    case 0x70: { // RNDSEED: reseed PRNG using V0..Vx packed
        if (!cpu.quirks.mx8_extensions) { cpu.halt(Chip8::HaltReason::UnknownOpcode); break; }
        uint64_t seed = 0;
        for (int i = 0; i <= x && i < 8; ++i) {
            seed = (seed << 8) | cpu.v[i];
        }
        cpu.setSeed(seed);
        break;
    }
    default:
        SchipISA::dispatchF(cpu, opcode);
        break;
    }
}

std::string Mx8ISA::dis5(uint16_t op) const {
    const uint8_t x = (op & 0x0F00) >> 8;
    const uint8_t y = (op & 0x00F0) >> 4;
    const uint8_t n =  op & 0x000F;
    switch (n) {
    case 0x1: return fmt("MUL V%X, V%X",  x, y);
    case 0x2: return fmt("DIV V%X, V%X",  x, y);
    case 0x3: return fmt("SWAP V%X, V%X", x, y);
    }
    return SchipISA::dis5(op);
}

std::string Mx8ISA::disF(uint16_t op) const {
    const uint8_t x  = (op & 0x0F00) >> 8;
    const uint8_t nn =  op & 0x00FF;
    if (nn == 0x50) return fmt("MEMCPY %X",  x);
    if (nn == 0x60) return fmt("MEMSET %X",  x);
    if (nn == 0x70) return fmt("RNDSEED %X", x);
    return SchipISA::disF(op);
}
