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
// Directives:
//   .equ NAME, expr         define a constant (.set is the same, and may
//   .set NAME, expr         redefine a name later in the file)
//   .org ADDR               continue at word address ADDR, padding with 0xFF
//   .byte a, b, c           emit bytes (a "..." operand emits its characters)
//   .word a, b              emit 16-bit little-endian words
//   .ascii "text"           emit the characters of a string
//   .asciz "text"           the same, NUL-terminated
//   .space N[, fill]        reserve N bytes, zero-filled unless told otherwise
//   .global NAME            accepted and ignored: there is no linker yet, so
//   .extern NAME            these only keep source portable
//
// A word always starts on a word boundary, so a run of data is padded with
// 0xFF (erased flash) before the next label, instruction, .org or the end of
// the image. Labels therefore always name word addresses.
//
// Operands: registers (r0-r31), decimal, 0x-hex, 0b-binary, 'c' characters,
// labels, .equ constants, and expressions over them using
// + - * / % & | ^ ~ << >> and parentheses. lo8(expr) and hi8(expr) take the
// low and high byte of a value, which is how a 16-bit address is loaded into
// a register pair with two ldi instructions.
AssembleResult assemble(std::string_view source);

} // namespace ardio
