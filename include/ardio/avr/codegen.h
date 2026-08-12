#pragma once
#include <map>
#include <string>

#include "ardio/avr/ast.h"

// The AVR code generator.
//
// CodeGen turns the shared AST into assembly text that the in-tree assembler
// accepts. The class is deliberately split across several translation units:
// this header declares the whole surface, expression generation lives in
// codegen_expr.cpp, statement generation and class methods live in their own
// files. Nothing here is virtual; the pieces simply share one object.
//
// Register conventions follow avr-gcc's ABI:
//   * r1 is permanently zero, r0 is scratch (the multiplier writes both).
//   * Arguments are allocated from register 26 downwards: each argument takes
//     its size rounded up to an even number of bytes, so the first int lands in
//     r24:r25, the second in r22:r23, the third in r20:r21, down to r8.
//   * Return values: 8-bit in r24, 16-bit in r24:r25, 32-bit in r22..r25.
//   * Call-clobbered: r18-r27, r30, r31. Call-saved: r2-r17, r28, r29.
//   * Y (r28:r29) is the frame pointer; locals are addressed as Y+q.
//
// Every expression leaves its value in r24 (8-bit) or r24:r25 (16-bit).

namespace ardio {

class CodeGen {
public:
    // ---- output -----------------------------------------------------------
    std::string out;                        // accumulated assembly text
    std::string error;                      // non-empty once something failed

    void emit(const std::string& line);     // appends the line and a newline
    void emit_label(const std::string& name);
    std::string new_label(const char* hint);
    void fail(const std::string& message);  // records the first error only
    bool failed() const { return !error.empty(); }

    // ---- generation -------------------------------------------------------

    // Evaluates expr, leaving its value in r24 (8-bit) or r24:r25 (16-bit).
    void gen_expr(const Expr& e);

    // Implemented in the statement generator.
    void gen_stmt(const Stmt& s);

    // Emits a complete function: label, prologue, body, epilogue. Assigns
    // frame slots for parameters and locals, and spills incoming argument
    // registers into the frame.
    void gen_function(const Function& f);

    // Implemented in the class/method generator.
    void gen_class_method(const ClassDecl& c, const Function& f);

    // ---- frame and locals, used by the statement generator ----------------

    // Byte displacement of a local from Y, or -1 if the name is not a local.
    int  local_offset(const std::string& name) const;
    void set_local_offset(const std::string& name, int offset);
    void clear_locals();
    int  frame_size = 0;

    // ---- globals ----------------------------------------------------------

    // Globals live at fixed SRAM addresses because the assembler's LDS and STS
    // forms take a literal address rather than a symbol.
    int  global_address(const std::string& name) const;   // -1 if unknown
    void set_global_address(const std::string& name, int address);
    int  add_global(const std::string& name, int size);   // allocates and returns
    int  next_global_address = 0x0100;                    // start of SRAM

    // ---- helpers shared with the other generator files --------------------

    // Size in bytes an expression evaluates to (1 or 2; defaults to 2).
    static int expr_size(const Expr& e);
    static bool expr_is_signed(const Expr& e);

    // Widens an 8-bit value already in r24 to a full 16-bit r24:r25.
    void widen_to_16(bool is_signed);

    // Marshals a call's arguments into the ABI registers and calls the target.
    void gen_call(const Expr& e);

    // Generates one binary operation. The result width and signedness are
    // passed separately so compound assignments can reuse this directly.
    void gen_binary(const std::string& op, const Expr& lhs, const Expr& rhs,
                    int size, bool is_signed);

    // Stores r24:r25 (or just r24 for size 1) into the named variable.
    void store_to_variable(const std::string& name, int size);
    void load_from_variable(const std::string& name, int size, bool is_signed);

private:
    int label_counter_ = 0;
    std::map<std::string, int> locals_;
    std::map<std::string, int> globals_;
};

} // namespace ardio
