// Expression code generation for the AVR back end.
//
// Every expression is evaluated into r24 (8-bit values) or r24:r25 (16-bit
// values), matching where avr-gcc leaves a return value. Binary operators
// evaluate their left side, push it, evaluate their right side into r22:r23
// and pop the left side back, so nesting depth is bounded only by the stack.
//
// Only registers r18 and above are used for immediates, because LDI, SUBI,
// SBCI, ANDI, ORI and CPI can only target r16-r31. r0 and r1 are touched by
// MUL alone, and r1 is cleared straight afterwards so it stays the zero
// register the ABI promises.

#include "ardio/avr/codegen.h"

namespace ardio {
namespace {

// A signed value rendered for the assembler's number parser, which accepts a
// leading '-' followed by decimal digits.
std::string imm(long v) { return std::to_string(v); }

std::string lo_byte(long v) { return imm(v & 0xFF); }
std::string hi_byte(long v) { return imm((v >> 8) & 0xFF); }

bool is_comparison(const std::string& op) {
    return op == "==" || op == "!=" || op == "<" || op == ">" ||
           op == "<=" || op == ">=";
}

} // namespace

// ------------------------------------------------------------- plumbing ----

void CodeGen::emit(const std::string& line) { out += line; out += '\n'; }

void CodeGen::emit_label(const std::string& name) { emit(name + ":"); }

std::string CodeGen::new_label(const char* hint) {
    return std::string(".L") + hint + std::to_string(label_counter_++);
}

void CodeGen::fail(const std::string& message) {
    if (error.empty()) error = message;
}

int CodeGen::local_offset(const std::string& name) const {
    auto it = locals_.find(name);
    return it == locals_.end() ? -1 : it->second;
}

void CodeGen::set_local_offset(const std::string& name, int offset) {
    locals_[name] = offset;
}

void CodeGen::clear_locals() { locals_.clear(); frame_size = 0; }

int CodeGen::global_address(const std::string& name) const {
    auto it = globals_.find(name);
    return it == globals_.end() ? -1 : it->second;
}

void CodeGen::set_global_address(const std::string& name, int address) {
    globals_[name] = address;
}

int CodeGen::add_global(const std::string& name, int size) {
    auto it = globals_.find(name);
    if (it != globals_.end()) return it->second;
    int address = next_global_address;
    next_global_address += size < 1 ? 1 : size;
    globals_[name] = address;
    return address;
}

int CodeGen::expr_size(const Expr& e) {
    if (!e.type) return 2;
    int n = e.type->size();
    return n < 1 ? 1 : (n > 2 ? 2 : n);
}

bool CodeGen::expr_is_signed(const Expr& e) {
    return e.type ? e.type->is_signed : true;
}

// -------------------------------------------------------------- widening ---

void CodeGen::widen_to_16(bool is_signed) {
    if (is_signed) {
        emit("clr r25");
        emit("sbrc r24, 7");
        emit("com r25");
    } else {
        emit("clr r25");
    }
}

// ------------------------------------------------------- variable access ---

void CodeGen::load_from_variable(const std::string& name, int size, bool is_signed) {
    int off = local_offset(name);
    if (off >= 0) {
        if (off > 62 || (size == 2 && off + 1 > 63)) {
            fail("local '" + name + "' is too far from the frame pointer");
            return;
        }
        emit("ldd r24, Y+" + imm(off));
        if (size == 2) emit("ldd r25, Y+" + imm(off + 1));
    } else {
        int addr = global_address(name);
        if (addr < 0) addr = add_global(name, size);
        emit("lds r24, " + imm(addr));
        if (size == 2) emit("lds r25, " + imm(addr + 1));
    }
    if (size == 1) widen_to_16(is_signed);
}

void CodeGen::store_to_variable(const std::string& name, int size) {
    int off = local_offset(name);
    if (off >= 0) {
        if (off > 62 || (size == 2 && off + 1 > 63)) {
            fail("local '" + name + "' is too far from the frame pointer");
            return;
        }
        emit("std Y+" + imm(off) + ", r24");
        if (size == 2) emit("std Y+" + imm(off + 1) + ", r25");
    } else {
        int addr = global_address(name);
        if (addr < 0) addr = add_global(name, size);
        emit("sts " + imm(addr) + ", r24");
        if (size == 2) emit("sts " + imm(addr + 1) + ", r25");
    }
}

// ------------------------------------------------------------------ calls --

void CodeGen::gen_call(const Expr& e) {
    // Arguments are evaluated left to right onto the stack, then popped into
    // their ABI registers: register 26 minus the running total of each
    // argument's size rounded up to an even number of bytes.
    std::vector<int> arg_regs;
    int reg = 26;
    for (const ExprPtr& a : e.args) {
        int size = a ? expr_size(*a) : 2;
        int slot = size <= 2 ? 2 : ((size + 1) & ~1);
        reg -= slot;
        if (reg < 8) { fail("too many arguments to '" + e.name + "'"); return; }
        arg_regs.push_back(reg);
    }

    for (const ExprPtr& a : e.args) {
        if (!a) { fail("null argument in call to '" + e.name + "'"); return; }
        gen_expr(*a);
        if (failed()) return;
        emit("push r24");
        emit("push r25");
    }

    for (size_t i = e.args.size(); i-- > 0;) {
        int r = arg_regs[i];
        emit("pop r" + imm(r + 1));
        emit("pop r" + imm(r));
    }

    emit("call " + e.name);
}

// ------------------------------------------------------------ expressions --

namespace {

// Emits the compare that leaves flags describing lhs (r24:r25) against rhs
// (r22:r23), then the branch mnemonic that is taken when the condition holds.
struct Compare {
    const char* branch;
    bool swap;
};

Compare comparison_branch(const std::string& op, bool is_signed) {
    if (op == "==") return {"breq", false};
    if (op == "!=") return {"brne", false};
    if (op == "<")  return {is_signed ? "brlt" : "brlo", false};
    if (op == ">=") return {is_signed ? "brge" : "brsh", false};
    if (op == ">")  return {is_signed ? "brlt" : "brlo", true};
    return {is_signed ? "brge" : "brsh", true};   // "<="
}

} // namespace

void CodeGen::gen_expr(const Expr& e) {
    if (failed()) return;

    switch (e.kind) {

    // ---- literals ---------------------------------------------------------
    case ExprKind::IntLiteral: {
        emit("ldi r24, " + lo_byte(e.int_value));
        emit("ldi r25, " + hi_byte(e.int_value));
        return;
    }

    case ExprKind::StringLiteral:
        fail("string literals are not supported in expressions yet");
        return;

    // ---- names ------------------------------------------------------------
    case ExprKind::Identifier:
        load_from_variable(e.name, expr_size(e), expr_is_signed(e));
        return;

    // ---- casts ------------------------------------------------------------
    case ExprKind::Cast: {
        if (!e.lhs) { fail("cast without an operand"); return; }
        gen_expr(*e.lhs);
        if (failed()) return;
        if (expr_size(e) == 1) {
            // Truncating: keep the low byte, then re-widen for the caller.
            widen_to_16(expr_is_signed(e));
        }
        return;
    }

    // ---- calls ------------------------------------------------------------
    case ExprKind::Call:
        gen_call(e);
        return;

    // ---- assignment -------------------------------------------------------
    case ExprKind::Assign: {
        if (!e.lhs || !e.rhs) { fail("malformed assignment"); return; }
        if (e.lhs->kind != ExprKind::Identifier) {
            fail("only simple variables can be assigned so far");
            return;
        }
        int size = expr_size(*e.lhs);
        if (e.op.empty() || e.op == "=") {
            gen_expr(*e.rhs);
            if (failed()) return;
        } else {
            // Compound assignment: run the plain binary operation with the
            // destination as its left operand.
            gen_binary(e.op.substr(0, e.op.size() - 1), *e.lhs, *e.rhs, size,
                       expr_is_signed(*e.lhs));
            if (failed()) return;
        }
        store_to_variable(e.lhs->name, size);
        return;
    }

    // ---- unary ------------------------------------------------------------
    case ExprKind::Unary: {
        if (!e.lhs) { fail("unary operator without an operand"); return; }
        const std::string& op = e.op;

        if (op == "++" || op == "--") {
            if (e.lhs->kind != ExprKind::Identifier) {
                fail("only simple variables can be incremented so far");
                return;
            }
            int size = expr_size(*e.lhs);
            load_from_variable(e.lhs->name, size, expr_is_signed(*e.lhs));
            if (failed()) return;
            if (e.is_postfix) { emit("push r24"); emit("push r25"); }
            if (op == "++") {
                emit("adiw r24, 1");
            } else {
                emit("sbiw r24, 1");
            }
            store_to_variable(e.lhs->name, size);
            if (failed()) return;
            if (e.is_postfix) { emit("pop r25"); emit("pop r24"); }
            return;
        }

        gen_expr(*e.lhs);
        if (failed()) return;

        if (op == "+") return;
        if (op == "-") {
            if (expr_size(e) == 1) {
                emit("neg r24");
                widen_to_16(expr_is_signed(e));
            } else {
                emit("com r25");
                emit("neg r24");
                emit("sbci r25, -1");
            }
            return;
        }
        if (op == "~") {
            emit("com r24");
            emit("com r25");
            return;
        }
        if (op == "!") {
            std::string done = new_label("not");
            emit("or r24, r25");
            emit("ldi r24, 0");
            emit("ldi r25, 0");
            emit(std::string("brne ") + done);
            emit("ldi r24, 1");
            emit_label(done);
            return;
        }
        fail("unsupported unary operator '" + op + "'");
        return;
    }

    // ---- conditional ------------------------------------------------------
    case ExprKind::Conditional: {
        if (!e.lhs || !e.rhs || !e.third) { fail("malformed conditional"); return; }
        std::string else_label = new_label("celse");
        std::string end_label = new_label("cend");
        gen_expr(*e.lhs);
        if (failed()) return;
        emit("or r24, r25");
        emit("breq " + else_label);
        gen_expr(*e.rhs);
        if (failed()) return;
        emit("rjmp " + end_label);
        emit_label(else_label);
        gen_expr(*e.third);
        if (failed()) return;
        emit_label(end_label);
        return;
    }

    // ---- binary -----------------------------------------------------------
    case ExprKind::Binary:
        if (!e.lhs || !e.rhs) { fail("malformed binary expression"); return; }
        gen_binary(e.op, *e.lhs, *e.rhs, expr_size(e), expr_is_signed(e));
        return;

    default:
        fail("unsupported expression kind");
        return;
    }
}

void CodeGen::gen_binary(const std::string& op, const Expr& lhs, const Expr& rhs,
                         int size, bool result_signed) {
    if (failed()) return;
    if (size < 1) size = 1;
    if (size > 2) size = 2;

    // Short-circuit operators never evaluate the right side unconditionally.
    if (op == "&&" || op == "||") {
        std::string shortcut = new_label(op == "&&" ? "andfalse" : "ortrue");
        std::string end_label = new_label("logend");
        gen_expr(lhs);
        if (failed()) return;
        emit("or r24, r25");
        emit(std::string(op == "&&" ? "breq " : "brne ") + shortcut);
        gen_expr(rhs);
        if (failed()) return;
        emit("or r24, r25");
        emit(std::string(op == "&&" ? "breq " : "brne ") + shortcut);
        emit(std::string("ldi r24, ") + (op == "&&" ? "1" : "0"));
        emit("ldi r25, 0");
        emit("rjmp " + end_label);
        emit_label(shortcut);
        emit(std::string("ldi r24, ") + (op == "&&" ? "0" : "1"));
        emit("ldi r25, 0");
        emit_label(end_label);
        return;
    }

    // Everything else: left in r24:r25, right in r22:r23.
    gen_expr(lhs);
    if (failed()) return;
    emit("push r24");
    emit("push r25");
    gen_expr(rhs);
    if (failed()) return;
    emit("movw r22, r24");
    emit("pop r25");
    emit("pop r24");

    const bool is_signed = result_signed && expr_is_signed(lhs);

    if (op == "+") {
        emit("add r24, r22");
        if (size == 2) emit("adc r25, r23");
        else widen_to_16(is_signed);
        return;
    }
    if (op == "-") {
        emit("sub r24, r22");
        if (size == 2) emit("sbc r25, r23");
        else widen_to_16(is_signed);
        return;
    }
    if (op == "*") {
        if (size == 1) {
            emit("mul r24, r22");
            emit("mov r24, r0");
            emit("clr r1");
            widen_to_16(is_signed);
        } else {
            emit("mul r24, r22");     // low * low
            emit("movw r18, r0");
            emit("mul r25, r22");     // high(left) * low(right)
            emit("add r19, r0");
            emit("mul r24, r23");     // low(left) * high(right)
            emit("add r19, r0");
            emit("clr r1");
            emit("movw r24, r18");
        }
        return;
    }
    if (op == "&" || op == "|" || op == "^") {
        const char* mnemonic = op == "&" ? "and" : (op == "|" ? "or" : "eor");
        emit(std::string(mnemonic) + " r24, r22");
        if (size == 2) emit(std::string(mnemonic) + " r25, r23");
        else widen_to_16(is_signed);
        return;
    }
    if (op == "<<" || op == ">>") {
        // The shift count is in r22; loop, because AVR shifts one bit at a time.
        std::string top = new_label("shift");
        std::string done = new_label("shiftend");
        emit_label(top);
        emit("tst r22");
        emit("breq " + done);
        if (op == "<<") {
            emit("lsl r24");
            if (size == 2) emit("rol r25");
        } else {
            if (size == 2) {
                emit(is_signed ? "asr r25" : "lsr r25");
                emit("ror r24");
            } else {
                emit(is_signed ? "asr r24" : "lsr r24");
            }
        }
        emit("dec r22");
        emit("rjmp " + top);
        emit_label(done);
        if (size == 1) widen_to_16(is_signed);
        return;
    }
    if (is_comparison(op)) {
        // Comparisons are made at the width of the operands, not of the result.
        int width = expr_size(lhs);
        bool cmp_signed = expr_is_signed(lhs) && expr_is_signed(rhs);
        Compare c = comparison_branch(op, cmp_signed);
        std::string done = new_label("cmp");
        if (c.swap) {
            emit("cp r22, r24");
            if (width == 2) emit("cpc r23, r25");
        } else {
            emit("cp r24, r22");
            if (width == 2) emit("cpc r25, r23");
        }
        emit("ldi r24, 1");
        emit("ldi r25, 0");
        emit(std::string(c.branch) + " " + done);
        emit("ldi r24, 0");
        emit_label(done);
        return;
    }

    fail("unsupported binary operator '" + op + "'");
}

} // namespace ardio
