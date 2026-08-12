#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace ardio {

struct CompileResult {
    bool ok = false;
    std::string error;      // includes a line number when known
    std::string assembly;   // AVR assembly, ready for assemble()
};

// Compiles a C/C++ subset to AVR assembly, following avr-gcc's ABI:
// arguments and return values in r24/r25 downward, r1 held at zero, Y as the
// frame pointer, r18-r27/r30/r31 call-clobbered.
//
// Supported: int/char/bool/void, global and local variables, functions with
// parameters, if/else, while, for, return, the usual arithmetic, comparison,
// logical and assignment operators, and calls.
//
// An Arduino-style sketch defining setup() and loop() gets the usual startup:
// setup() runs once, then loop() forever. A program defining main() gets that
// called instead.
//
// Not supported yet: classes, templates, pointers, arrays, floating point,
// 32-bit types. These are reported as errors rather than silently miscompiled.
CompileResult compile_avr(std::string_view source);

// As above, but runs the C preprocessor first, resolving #include against
// `include_paths` and expanding macros. Pass the directory holding ardio's
// Arduino-compatible headers to compile a sketch that includes them.
CompileResult compile_avr(std::string_view source,
                          const std::vector<std::string>& include_paths);

} // namespace ardio
