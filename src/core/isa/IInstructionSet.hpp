#pragma once

#include <cstdint>
#include <string>

class Chip8;

// Pluggable instruction-set interface.
//
// Each ISA implementation owns exactly three responsibilities:
//   1. execute(cpu, opcode)  — runs one opcode against the CPU's public API
//   2. disassemble(opcode)   — renders a mnemonic for the debugger
//   3. name()                — diagnostic label
//
// The ISA does NOT own state. CPU registers, memory, stack, timers, RNG,
// keys live on the Chip8 object. The ISA is a stateless dispatcher — you
// can swap it mid-run as long as the bytecode remains valid for the new
// ISA. (In practice we swap on ROM load, not mid-execution.)
//
// Composition pattern: SchipISA inherits from Chip8ISA and only overrides
// the slots SCHIP extends. Mx8ISA and Xo8ISA are SIBLINGS — both inherit
// from SchipISA, not from each other, because MX-8 and XO-CHIP both claim
// the 5XY2/5XY3 opcode slots with incompatible meanings (MX-8: DIV/SWAP;
// XO-CHIP: save/load vx-vy). Keeping them sibling subclasses makes the two
// extension sets mutually exclusive, exactly as a real machine would treat
// them. New experiments can subclass any of these.
class IInstructionSet {
public:
    virtual ~IInstructionSet() = default;

    // Decode + dispatch one instruction. The CPU has already incremented PC
    // past the opcode bytes; ISA mutates state via the CPU's public surface.
    // On unknown / unsupported opcode the ISA must call cpu.halt(...).
    virtual void execute(Chip8& cpu, uint16_t opcode) = 0;

    // Render a 16-32 char mnemonic. Pure function — no CPU state needed.
    virtual std::string disassemble(uint16_t opcode) const = 0;

    // Stable short identifier ("CHIP-8", "SCHIP", "MX-8").
    virtual const char* name() const = 0;
};

// Singleton accessors for the built-in ISAs. Returned references are stable
// for the lifetime of the program; safe to store as IInstructionSet*.
namespace ISA {
    IInstructionSet& chip8();
    IInstructionSet& schip();
    IInstructionSet& mx8();
    IInstructionSet& xochip();

    // Pick by name (case-insensitive). Falls back to schip() on miss.
    IInstructionSet& by_name(const std::string& name);
}
