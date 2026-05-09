#pragma once

#include "SchipISA.hpp"

// MX-8 instruction set — composes SCHIP and adds 6 custom opcodes.
// All MX-8 ops are gated by quirks.mx8_extensions; with the quirk off
// they halt as UnknownOpcode (so an Mx8ISA instance is safe to leave
// installed for any ROM, including pure CHIP-8 / SCHIP).
//
// Added opcodes:
//   5XY1   MUL Vx, Vy        Vx = (Vx*Vy) & 0xFF;  VF = high byte
//   5XY2   DIV Vx, Vy        Vx = Vx/Vy;  VF = Vx%Vy   (VF=0xFF if Vy=0)
//   5XY3   SWAP Vx, Vy
//   FX50   MEMCPY (X+1) bytes from [I] to [I + X+1]
//   FX60   MEMSET (X+1) bytes at [I] to V0
//   FX70   RNDSEED — reseed PRNG using V0..Vx packed
class Mx8ISA : public SchipISA {
public:
    const char* name() const override { return "MX-8"; }

protected:
    void        dispatch5(Chip8& cpu, uint16_t opcode) override;
    void        dispatchF(Chip8& cpu, uint16_t opcode) override;

    std::string dis5(uint16_t opcode) const override;
    std::string disF(uint16_t opcode) const override;
};
