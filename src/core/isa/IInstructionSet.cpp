#include "IInstructionSet.hpp"
#include "Chip8ISA.hpp"
#include "SchipISA.hpp"
#include "Mx8ISA.hpp"
#include "Xo8ISA.hpp"

#include <algorithm>
#include <cctype>

namespace ISA {

IInstructionSet& chip8()  { static Chip8ISA s; return s; }
IInstructionSet& schip()  { static SchipISA s; return s; }
IInstructionSet& mx8()    { static Mx8ISA   s; return s; }
IInstructionSet& xochip() { static Xo8ISA   s; return s; }

IInstructionSet& by_name(const std::string& name) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (n == "chip8" || n == "chip-8")   return chip8();
    if (n == "schip" || n == "super-chip" || n == "schip1.1") return schip();
    if (n == "mx8"   || n == "mx-8")     return mx8();
    if (n == "xochip" || n == "xo-chip" || n == "xo")         return xochip();
    return schip();    // sensible default
}

} // namespace ISA
