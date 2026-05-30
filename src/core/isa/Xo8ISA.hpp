#pragma once

#include "SchipISA.hpp"

// XO-CHIP instruction set — Octo's extension of SUPER-CHIP. A SIBLING of
// Mx8ISA (both extend SchipISA), never composed with it: XO-CHIP and MX-8
// give incompatible meanings to 5XY2/5XY3, so a machine runs one or the
// other, not both. See IInstructionSet.hpp for the rationale.
//
// Added / changed opcodes (relative to SUPER-CHIP):
//   00DN        scroll-up N lines        (plane-aware)
//   00CN        scroll-down N lines      (plane-aware; overrides SCHIP)
//   00FB / 00FC scroll right / left 4px  (plane-aware; overrides SCHIP)
//   5XY2        save vx-vy  -> store V_X..V_Y to [I], I unchanged
//   5XY3        load vx-vy  -> load  V_X..V_Y from [I], I unchanged
//   F000 NNNN   i := long NNNN  (4-byte op; loads a full 16-bit address)
//   FN01        plane N         (select draw plane bitmask, N in 0..3)
//   F002        audio           (load 16-byte audio pattern from [I])
//   FX3A        pitch vx        (set audio playback pitch = Vx)
//   DXY0 (hires) plane-aware 16x16 sprite (handled in Chip8::drawSprite)
//
// XO-CHIP does NOT gate its opcodes behind a runtime quirk the way MX-8 does;
// selecting the XO-CHIP ISA *is* the opt-in. A ROM that wants these features
// is loaded with --isa xochip (or the F-key toggle).
class Xo8ISA : public SchipISA {
public:
    const char* name() const override { return "XO-CHIP"; }

protected:
    void        dispatch0(Chip8& cpu, uint16_t opcode) override;
    void        dispatch5(Chip8& cpu, uint16_t opcode) override;
    void        dispatchF(Chip8& cpu, uint16_t opcode) override;

    std::string dis0(uint16_t opcode) const override;
    std::string dis5(uint16_t opcode) const override;
    std::string disF(uint16_t opcode) const override;
};
