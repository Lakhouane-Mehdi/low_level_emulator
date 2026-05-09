#include "Chip8ISA.hpp"

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

// ============== execute ==============

void Chip8ISA::execute(Chip8& cpu, uint16_t opcode) {
    const uint16_t nnn = opcode & 0x0FFF;
    const uint8_t  nn  = opcode & 0x00FF;
    const uint8_t  x   = (opcode & 0x0F00) >> 8;
    const uint8_t  y   = (opcode & 0x00F0) >> 4;
    const uint8_t  n   =  opcode & 0x000F;

    switch (opcode & 0xF000) {
    case 0x0000: dispatch0(cpu, opcode); break;
    case 0x1000: cpu.pc = nnn; break;
    case 0x2000: cpu.pushStack(cpu.pc); cpu.pc = nnn; break;
    case 0x3000: if (cpu.v[x] == nn)         cpu.pc += 2; break;
    case 0x4000: if (cpu.v[x] != nn)         cpu.pc += 2; break;
    case 0x5000: dispatch5(cpu, opcode);     break;
    case 0x6000: cpu.v[x]  = nn;             break;
    case 0x7000: cpu.v[x] += nn;             break;
    case 0x8000: dispatch8(cpu, opcode);     break;
    case 0x9000: if (cpu.v[x] != cpu.v[y])   cpu.pc += 2; break;
    case 0xA000: cpu.index = nnn;            break;
    case 0xB000:
        // BNNN: classic = PC = V0 + nnn ; SCHIP quirk = PC = VX + nnn.
        cpu.pc = nnn + cpu.v[cpu.quirks.jump_with_offset_x ? x : 0];
        break;
    case 0xC000: cpu.v[x] = static_cast<uint8_t>(cpu.rng.next() & 0xFF) & nn; break;
    case 0xD000: cpu.drawSprite(x, y, n);    break;
    case 0xE000: dispatchE(cpu, opcode);     break;
    case 0xF000: dispatchF(cpu, opcode);     break;
    default:     cpu.halt(Chip8::HaltReason::UnknownOpcode); break;
    }
}

// ---- 0x0NNN: base CHIP-8 only knows CLS and RET. SCHIP overrides to add
// scrolls / hires / EXIT.
void Chip8ISA::dispatch0(Chip8& cpu, uint16_t opcode) {
    const uint8_t nn = opcode & 0x00FF;
    switch (nn) {
    case 0xE0: cpu.clearScreen(); break;
    case 0xEE: cpu.pc = cpu.popStack(); break;
    default:   cpu.halt(Chip8::HaltReason::UnknownOpcode); break;
    }
}

// ---- 0x5XY*: base only knows 5XY0 (SE Vx, Vy). MX-8 overrides for 5XY1/2/3.
void Chip8ISA::dispatch5(Chip8& cpu, uint16_t opcode) {
    const uint8_t x = (opcode & 0x0F00) >> 8;
    const uint8_t y = (opcode & 0x00F0) >> 4;
    const uint8_t n =  opcode & 0x000F;
    if (n == 0) {
        if (cpu.v[x] == cpu.v[y]) cpu.pc += 2;
    } else {
        cpu.halt(Chip8::HaltReason::UnknownOpcode);
    }
}

// ---- 0x8XYN: ALU group. All quirk handling lives here.
void Chip8ISA::dispatch8(Chip8& cpu, uint16_t opcode) {
    const uint8_t x = (opcode & 0x0F00) >> 8;
    const uint8_t y = (opcode & 0x00F0) >> 4;
    const uint8_t n =  opcode & 0x000F;
    auto& v = cpu.v;
    const auto& q = cpu.quirks;

    switch (n) {
    case 0x0: v[x]  = v[y]; break;
    case 0x1: v[x] |= v[y]; if (q.vf_reset) v[0xF] = 0; break;
    case 0x2: v[x] &= v[y]; if (q.vf_reset) v[0xF] = 0; break;
    case 0x3: v[x] ^= v[y]; if (q.vf_reset) v[0xF] = 0; break;
    case 0x4: {
        // Compute carry first; write VF last so x==0xF still yields the flag.
        uint16_t sum = v[x] + v[y];
        uint8_t flag = sum > 0xFF ? 1 : 0;
        v[x] = sum & 0xFF;
        v[0xF] = flag;
        break;
    }
    case 0x5: {
        uint8_t flag = v[x] >= v[y] ? 1 : 0;
        v[x] = v[x] - v[y];
        v[0xF] = flag;
        break;
    }
    case 0x6: {
        uint8_t src  = q.shift_in_place ? v[x] : v[y];
        uint8_t flag = src & 0x1;
        v[x] = src >> 1;
        v[0xF] = flag;
        break;
    }
    case 0x7: {
        uint8_t flag = v[y] >= v[x] ? 1 : 0;
        v[x] = v[y] - v[x];
        v[0xF] = flag;
        break;
    }
    case 0xE: {
        uint8_t src  = q.shift_in_place ? v[x] : v[y];
        uint8_t flag = (src & 0x80) >> 7;
        v[x] = static_cast<uint8_t>(src << 1);
        v[0xF] = flag;
        break;
    }
    default: cpu.halt(Chip8::HaltReason::UnknownOpcode); break;
    }
}

// ---- 0xEX9E / 0xEXA1: keypad skips.
void Chip8ISA::dispatchE(Chip8& cpu, uint16_t opcode) {
    const uint8_t x  = (opcode & 0x0F00) >> 8;
    const uint8_t nn =  opcode & 0x00FF;
    switch (nn) {
    case 0x9E: if ( cpu.keys[cpu.v[x] & 0xF]) cpu.pc += 2; break;
    case 0xA1: if (!cpu.keys[cpu.v[x] & 0xF]) cpu.pc += 2; break;
    default:   cpu.halt(Chip8::HaltReason::UnknownOpcode); break;
    }
}

// ---- 0xFXNN: timers / I / BCD / load-store. SCHIP and MX-8 add to this.
void Chip8ISA::dispatchF(Chip8& cpu, uint16_t opcode) {
    const uint8_t x  = (opcode & 0x0F00) >> 8;
    const uint8_t nn =  opcode & 0x00FF;
    auto& v = cpu.v;

    switch (nn) {
    case 0x07: v[x] = cpu.delay_timer; break;
    case 0x0A: {
        // FX0A — block until any key is held. Re-execute the instruction
        // each frame by undoing the PC advance until a press appears.
        bool any = false;
        for (uint8_t i = 0; i < Chip8::KEY_COUNT; ++i) {
            if (cpu.keys[i]) { v[x] = i; any = true; break; }
        }
        if (!any) cpu.pc -= 2;
        break;
    }
    case 0x15: cpu.delay_timer = v[x]; break;
    case 0x18: cpu.sound_timer = v[x]; break;
    case 0x1E: cpu.index += v[x]; break;
    case 0x29: cpu.index = Chip8::FONTSET_ADDRESS + (v[x] & 0xF) * 5; break;
    case 0x33:
        if (cpu.index + 2 >= Chip8::MEMORY_SIZE) {
            cpu.halt(Chip8::HaltReason::BadMemoryAccess); break;
        }
        cpu.mem.write(cpu.index,     v[x] / 100);
        cpu.mem.write(cpu.index + 1, (v[x] / 10) % 10);
        cpu.mem.write(cpu.index + 2, v[x] % 10);
        break;
    case 0x55:
        if (cpu.index + x >= Chip8::MEMORY_SIZE) {
            cpu.halt(Chip8::HaltReason::BadMemoryAccess); break;
        }
        for (int i = 0; i <= x; ++i) cpu.mem.write(cpu.index + i, v[i]);
        if (!cpu.quirks.load_store_no_inc) cpu.index += x + 1;
        break;
    case 0x65:
        if (cpu.index + x >= Chip8::MEMORY_SIZE) {
            cpu.halt(Chip8::HaltReason::BadMemoryAccess); break;
        }
        for (int i = 0; i <= x; ++i) v[i] = cpu.mem.read(cpu.index + i);
        if (!cpu.quirks.load_store_no_inc) cpu.index += x + 1;
        break;
    default:   cpu.halt(Chip8::HaltReason::UnknownOpcode); break;
    }
}

// ============== disassemble ==============

std::string Chip8ISA::disassemble(uint16_t op) const {
    const uint16_t nnn = op & 0x0FFF;
    const uint8_t  nn  = op & 0x00FF;
    const uint8_t  n   = op & 0x000F;
    const uint8_t  x   = (op & 0x0F00) >> 8;
    const uint8_t  y   = (op & 0x00F0) >> 4;

    switch (op & 0xF000) {
    case 0x0000: { auto s = dis0(op); if (!s.empty()) return s; return fmt("SYS %03X", nnn); }
    case 0x1000: return fmt("JP %03X", nnn);
    case 0x2000: return fmt("CALL %03X", nnn);
    case 0x3000: return fmt("SE V%X, %02X", x, nn);
    case 0x4000: return fmt("SNE V%X, %02X", x, nn);
    case 0x5000: { auto s = dis5(op); if (!s.empty()) return s; break; }
    case 0x6000: return fmt("LD V%X, %02X", x, nn);
    case 0x7000: return fmt("ADD V%X, %02X", x, nn);
    case 0x8000:
        switch (n) {
        case 0x0: return fmt("LD V%X, V%X",   x, y);
        case 0x1: return fmt("OR V%X, V%X",   x, y);
        case 0x2: return fmt("AND V%X, V%X",  x, y);
        case 0x3: return fmt("XOR V%X, V%X",  x, y);
        case 0x4: return fmt("ADD V%X, V%X",  x, y);
        case 0x5: return fmt("SUB V%X, V%X",  x, y);
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
    case 0xF000: { auto s = disF(op); if (!s.empty()) return s; break; }
    }
    return fmt("DW %04X", op);
}

std::string Chip8ISA::dis0(uint16_t op) const {
    if ((op & 0xFF) == 0xE0) return "CLS";
    if ((op & 0xFF) == 0xEE) return "RET";
    return {};
}

std::string Chip8ISA::dis5(uint16_t op) const {
    if ((op & 0xF) == 0) {
        const uint8_t x = (op & 0x0F00) >> 8;
        const uint8_t y = (op & 0x00F0) >> 4;
        return fmt("SE V%X, V%X", x, y);
    }
    return {};
}

std::string Chip8ISA::disF(uint16_t op) const {
    const uint8_t x  = (op & 0x0F00) >> 8;
    const uint8_t nn =  op & 0x00FF;
    switch (nn) {
    case 0x07: return fmt("LD V%X, DT",  x);
    case 0x0A: return fmt("LD V%X, K",   x);
    case 0x15: return fmt("LD DT, V%X",  x);
    case 0x18: return fmt("LD ST, V%X",  x);
    case 0x1E: return fmt("ADD I, V%X",  x);
    case 0x29: return fmt("LD F, V%X",   x);
    case 0x33: return fmt("LD B, V%X",   x);
    case 0x55: return fmt("LD [I], V%X", x);
    case 0x65: return fmt("LD V%X, [I]", x);
    }
    return {};
}
