#pragma once
#include <string>

#include "ardio/avr/ast.h"

// Semantic analysis for ardio's AVR compiler.
//
// The parser produces a tree whose Expr::type fields are all null and whose
// classes have no field offsets. analyse() walks that tree once and:
//
//   * lays out every class -- fields in declaration order, byte packed,
//     offsets from zero (AVR has no alignment requirement) -- filling in
//     Field::offset and ClassDecl::size;
//   * gives every expression a type, applying the usual arithmetic
//     conversions, decaying arrays to pointers at their use sites, and
//     scaling pointer arithmetic by the pointee size;
//   * resolves overloaded functions and methods -- several may share a source
//     name -- and stamps the chosen one's unique symbol onto the call;
//   * fills in default arguments a call left out, so later stages only ever
//     see a complete argument list;
//   * rejects undeclared names, calls with the wrong number of arguments,
//     and assignments to things that are not lvalues.
//
// Analysis stops at the first error, which is reported as "line N: <problem>".

namespace ardio {

struct SemaResult {
    bool ok = false;
    std::string error;
};

SemaResult analyse(Program& program);

// Class sizes live beside the type code so that Type::size() can answer for a
// TypeKind::Class without every Type carrying a back-pointer to its
// declaration. The layout pass in analyse() populates the table.
void set_class_size(const std::string& name, int size);
int lookup_class_size(const std::string& name);
void clear_class_sizes();

// Default arguments live in a side table for the same reason class sizes do:
// ast.h's Param has no field to hold one, and the AST is shared with modules
// that have no interest in defaults. The front end records a default here as
// it parses the declaration; analyse() then materialises the value as a real
// argument at every call that leaves it out, so code generation sees an
// ordinary complete call and needs to know nothing about defaults.
//
// A default is restricted to an integer constant. That covers the published
// Arduino API -- Servo::attach(pin, min = 544, max = 2400), random(min, max),
// Print::print(value, base = DEC) -- and keeps the recorded value something
// analyse() can rebuild without dragging a parsed expression across module
// boundaries.
//
// `owner_class` is empty for a free function. `param_count` is the declared
// parameter count of the function being described, which is what tells two
// overloads of one name apart. `param_index` is 0-based, and defaults must be
// trailing: a parameter is only optional if every parameter after it is too.
void set_default_argument(const std::string& owner_class, const std::string& name,
                          size_t param_count, size_t param_index, long value);
void clear_default_arguments();

} // namespace ardio
