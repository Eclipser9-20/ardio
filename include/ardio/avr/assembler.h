#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ardio {

struct AssembleResult {
    bool ok = false;
    std::string error;               // includes a line number when known
    std::vector<uint8_t> code;       // flash image, little-endian words
};

// Assembles AVR source into a flat flash image starting at address 0.
//
// Supported syntax:
//   label:                  a label, alone or before an instruction
//   mnemonic op[, op]       one instruction
//   ; comment               to end of line ('#' also works)
//
// Operands: registers (r0-r31), decimal, 0x-hex, 0b-binary, and labels.
AssembleResult assemble(std::string_view source);

} // namespace ardio
