#pragma once
#include <string>
#include <vector>

#include "ardio/avr/ast.h"
#include "ardio/avr/token.h"

// Recursive-descent parser for the C++ subset ardio compiles for AVR targets.
//
// The parser consumes the token vector produced by tokenize() and builds the
// shared Program tree declared in ast.h. It performs no name resolution and no
// type checking: Expr::type is left null for semantic analysis to fill in.
//
// Parsing stops at the first error. The message always carries the source line
// so the driver can print it verbatim:
//
//     line 12: expected ';' after expression

namespace ardio {

struct ParseResult {
    bool ok = false;
    std::string error;
    Program program;
};

ParseResult parse(const std::vector<Token>& tokens);

} // namespace ardio
