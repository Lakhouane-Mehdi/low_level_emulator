#pragma once

#include "Chip8ISA.hpp"

// SUPER-CHIP 1.1 ISA. Extends base CHIP-8 with:
//   - 00CN / 00DN scroll down/up (N lines)
//   - 00FB / 00FC scroll right/left 4 pixels
//   - 00FD EXIT
//   - 00FE / 00FF lo-res / hi-res mode
//   - FX30 LD HF, Vx  (big font 8x10 for digits 0-9)
//   - FX75 / FX85     RPL flag store/load (8 user flags)
//   - DXY0 16x16 sprite (handled by Chip8::drawSprite when n==0 + hires)
class SchipISA : public Chip8ISA {
public:
    const char* name() const override { return "SCHIP"; }

protected:
    void        dispatch0(Chip8& cpu, uint16_t opcode) override;
    void        dispatchF(Chip8& cpu, uint16_t opcode) override;

    std::string dis0(uint16_t opcode) const override;
    std::string disF(uint16_t opcode) const override;
};
