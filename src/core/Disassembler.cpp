#include "Disassembler.hpp"
#include "isa/IInstructionSet.hpp"

// Free-function disassembler — kept for callers that don't know about ISAs
// (debug view, opcode tooltips). Always renders via the broadest ISA so
// every legal opcode in memory gets a mnemonic instead of "DW xxxx", even
// when the active execution ISA is narrower.
std::string disassemble(uint16_t op) {
    return ISA::mx8().disassemble(op);
}
