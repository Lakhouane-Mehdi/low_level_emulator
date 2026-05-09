#pragma once

#include "IInstructionSet.hpp"

class Chip8;

// Base CHIP-8 instruction set — the 35 original opcodes (Cosmac VIP / 1977).
//
// Subclasses hook the per-group dispatchers (dispatch0 / dispatch5 / dispatch8 /
// dispatchE / dispatchF) to extend or override behavior. The top-level
// execute() is final because every CHIP-8 family ISA shares the same
// fetch/decode-mask layout.
class Chip8ISA : public IInstructionSet {
public:
    void        execute(Chip8& cpu, uint16_t opcode) override;
    std::string disassemble(uint16_t opcode) const override;
    const char* name() const override { return "CHIP-8"; }

protected:
    // Per-group hooks. Subclasses override these to add SCHIP / MX-8 ops.
    virtual void dispatch0(Chip8& cpu, uint16_t opcode);
    virtual void dispatch5(Chip8& cpu, uint16_t opcode);   // 5XY0 base; MX-8 adds 5XY1/2/3
    virtual void dispatch8(Chip8& cpu, uint16_t opcode);
    virtual void dispatchE(Chip8& cpu, uint16_t opcode);
    virtual void dispatchF(Chip8& cpu, uint16_t opcode);

    // Disassembly hooks — return non-empty to short-circuit the base table.
    virtual std::string dis0(uint16_t opcode) const;
    virtual std::string dis5(uint16_t opcode) const;
    virtual std::string disF(uint16_t opcode) const;
};
