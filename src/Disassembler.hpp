#pragma once

#include <cstdint>
#include <string>

// Returns a short human-readable mnemonic for a CHIP-8 opcode,
// e.g. 0x6A02 -> "LD VA, 02"
std::string disassemble(uint16_t opcode);
