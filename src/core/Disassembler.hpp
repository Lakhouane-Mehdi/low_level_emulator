#pragma once

#include <cstdint>
#include <string>

// Returns a short SCHIP-style mnemonic for a CHIP-8 opcode.
// e.g. 0x6A02 -> "LD VA, 02"   0xD012 -> "DRW V0, V1, 2"
std::string disassemble(uint16_t opcode);
