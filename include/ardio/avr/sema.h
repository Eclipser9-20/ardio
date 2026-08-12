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

} // namespace ardio
