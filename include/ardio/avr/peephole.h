#pragma once
#include <string>

// A peephole optimiser for AVR assembly text.
//
// The code generator has no intermediate representation: it emits assembly
// straight from the AST, so the same value is often stored and immediately
// reloaded, or saved around a subexpression that never touches it. Rewriting
// the finished text is the cheapest place to clean that up.
//
// The pass only ever deletes an instruction or replaces a run of instructions
// with a shorter equivalent one. It never moves code, never rewrites across a
// label -- anything can jump to a label, so what reaches it is unknowable --
// and never removes an instruction whose flags a later branch might read. A
// pattern that cannot be proven safe from the surrounding text alone is left
// alone.
//
// Lines the optimiser does not understand (directives, unknown mnemonics,
// comments, blank lines) are preserved verbatim and act as barriers.

namespace ardio {

// Rewrites AVR assembly text into smaller, equivalent code.
//
// The result assembles to the same behaviour as the input, given the register
// conventions the code generator follows: r1 is permanently zero, r0 is
// scratch, and Y (r28:r29) is the frame pointer.
std::string optimise_assembly(const std::string& assembly);

} // namespace ardio
