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

#include <map>

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

// ---------------------------------------------------------- class layout ---
//
// Member access needs the byte offset the layout pass computed, but a Type
// carries only the class name, exactly as it carries only the name for
// Type::size(). The offsets therefore live in a table beside the code
// generator, populated from the analysed program before generation, mirroring
// how set_class_size() feeds Type::size().

namespace {

std::map<std::string, std::map<std::string, int>>& class_layouts() {
    static std::map<std::string, std::map<std::string, int>> layouts;
    return layouts;
}

} // namespace

void set_class_layout(const ClassDecl& c) {
    auto& fields = class_layouts()[c.name];
    for (const Field& f : c.fields) fields[f.name] = f.offset;
}

int class_field_offset(const std::string& class_name, const std::string& field) {
    auto cls = class_layouts().find(class_name);
    if (cls == class_layouts().end()) return -1;
    auto it = cls->second.find(field);
    return it == cls->second.end() ? -1 : it->second;
}

void clear_class_layouts() { class_layouts().clear(); }

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
    // A long is the one type wider than a register pair that the generator
    // materialises: it lives in r22..r25, exactly where the ABI puts a 32-bit
    // argument or return value. Everything else keeps the old clamp, so an
    // aggregate still yields the address-sized answer callers expect.
    if (e.type->kind == TypeKind::Long || e.type->kind == TypeKind::ULong) return 4;
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

// ------------------------------------------------------------ 32-bit core ---
//
// A value of one or two bytes lives in r24:r25 -- an 8-bit value is always
// kept sign- or zero-extended into r25, so in registers there are really only
// two widths. A 32-bit value lives in r22..r25, low byte first, which is both
// the ABI's home for a `long` argument and its home for a `long` return value.
// Nothing else moves: narrow code generation is byte-for-byte what it was.

// Widens r24:r25 into r22..r25, or narrows r22..r25 back into r24:r25.
// `from` and `to` are expr_size() answers; a width of 1 only differs from 2 in
// that the value is re-extended from its low byte afterwards.
void gen_convert(CodeGen& g, int from, int to, bool from_signed, bool to_signed) {
    if (g.failed()) return;
    const bool from_wide = from == 4;
    const bool to_wide = to == 4;

    if (from_wide && !to_wide) {
        g.emit("movw r24, r22");                 // keep the low half
        if (to == 1) g.widen_to_16(to_signed);
        return;
    }
    if (!from_wide && to_wide) {
        g.emit("movw r22, r24");                 // the value becomes the low half
        if (from_signed) {
            g.emit("clr r24");
            g.emit("sbrc r23, 7");
            g.emit("com r24");
            g.emit("mov r25, r24");
        } else {
            g.emit("clr r24");
            g.emit("clr r25");
        }
        return;
    }
    // Both narrow: a 2 -> 1 narrowing re-extends, everything else is a no-op
    // because an 8-bit value is already carried extended.
    if (!from_wide && to == 1) g.widen_to_16(to_signed);
}

// Sets Z from the value, without destroying it: used by if/while/for.
void gen_test_value(CodeGen& g, int size) {
    if (size == 4) {
        g.emit("    cp   r22, r1");
        g.emit("    cpc  r23, r1");
        g.emit("    cpc  r24, r1");
        g.emit("    cpc  r25, r1");
    } else {
        g.emit("    cp   r24, r1");
        g.emit("    cpc  r25, r1");
    }
}

namespace {

// Sets Z from a value that is being consumed, so it may be destroyed.
void test_and_consume(CodeGen& g, int size) {
    if (size == 4) {
        g.emit("or r22, r23");
        g.emit("or r22, r24");
        g.emit("or r22, r25");
    } else {
        g.emit("or r24, r25");
    }
}

void push_value(CodeGen& g, int size) {
    if (size == 4) { g.emit("push r22"); g.emit("push r23"); }
    g.emit("push r24");
    g.emit("push r25");
}

void pop_value(CodeGen& g, int size) {
    g.emit("pop r25");
    g.emit("pop r24");
    if (size == 4) { g.emit("pop r23"); g.emit("pop r22"); }
}

// r22..r25 = -r22..r25. COM sets the carry, so every complement has to happen
// before the NEG whose borrow the SBCI chain then propagates.
void negate32(CodeGen& g) {
    g.emit("com r25");
    g.emit("com r24");
    g.emit("com r23");
    g.emit("neg r22");
    g.emit("sbci r23, -1");
    g.emit("sbci r24, -1");
    g.emit("sbci r25, -1");
}

// r22..r25 += 1 or -= 1, using the SBCI borrow chain rather than four
// conditional branches.
void step32(CodeGen& g, bool up) {
    if (up) {
        g.emit("subi r22, -1");
        g.emit("sbci r23, -1");
        g.emit("sbci r24, -1");
        g.emit("sbci r25, -1");
    } else {
        g.emit("subi r22, 1");
        g.emit("sbci r23, 0");
        g.emit("sbci r24, 0");
        g.emit("sbci r25, 0");
    }
}

} // namespace

// ------------------------------------------------------- variable access ---

namespace {

// The registers a value of `size` bytes occupies, low byte first. ldd/std
// reach only 63 bytes past Y, so a 32-bit slot needs its last byte in range
// too -- that is checked by the callers below rather than assumed.
const char* const kWideRegs[4] = {"r22", "r23", "r24", "r25"};

bool frame_slot_in_range(CodeGen& g, const std::string& name, int off, int size) {
    if (off > 62 || off + size - 1 > 63) {
        g.fail("local '" + name + "' is too far from the frame pointer");
        return false;
    }
    return true;
}

} // namespace

void CodeGen::load_from_variable(const std::string& name, int size, bool is_signed) {
    const int bytes = size == 4 ? 4 : (size == 2 ? 2 : 1);
    int off = local_offset(name);
    if (off >= 0) {
        if (!frame_slot_in_range(*this, name, off, bytes)) return;
        if (bytes == 4) {
            for (int i = 0; i < 4; ++i)
                emit(std::string("ldd ") + kWideRegs[i] + ", Y+" + imm(off + i));
        } else {
            emit("ldd r24, Y+" + imm(off));
            if (bytes == 2) emit("ldd r25, Y+" + imm(off + 1));
        }
    } else {
        int addr = global_address(name);
        if (addr < 0) addr = add_global(name, bytes);
        if (bytes == 4) {
            for (int i = 0; i < 4; ++i)
                emit(std::string("lds ") + kWideRegs[i] + ", " + imm(addr + i));
        } else {
            emit("lds r24, " + imm(addr));
            if (bytes == 2) emit("lds r25, " + imm(addr + 1));
        }
    }
    if (bytes == 1) widen_to_16(is_signed);
}

void CodeGen::store_to_variable(const std::string& name, int size) {
    const int bytes = size == 4 ? 4 : (size == 2 ? 2 : 1);
    int off = local_offset(name);
    if (off >= 0) {
        if (!frame_slot_in_range(*this, name, off, bytes)) return;
        if (bytes == 4) {
            for (int i = 0; i < 4; ++i)
                emit("std Y+" + imm(off + i) + ", " + kWideRegs[i]);
        } else {
            emit("std Y+" + imm(off) + ", r24");
            if (bytes == 2) emit("std Y+" + imm(off + 1) + ", r25");
        }
    } else {
        int addr = global_address(name);
        if (addr < 0) addr = add_global(name, bytes);
        if (bytes == 4) {
            for (int i = 0; i < 4; ++i)
                emit("sts " + imm(addr + i) + ", " + kWideRegs[i]);
        } else {
            emit("sts " + imm(addr) + ", r24");
            if (bytes == 2) emit("sts " + imm(addr + 1) + ", r25");
        }
    }
}

// ------------------------------------------------------------------ calls --

// Defined below: leaves an lvalue's address in r24:r25.
namespace { bool gen_address(CodeGen& g, const Expr& e); }

void CodeGen::gen_call(const Expr& e) {
    // A method call carries the object in `lhs`. `this` is the implicit first
    // argument, so it takes r24:r25 and pushes the declared arguments down one
    // slot each. The label is ClassName__method, because the assembler has no
    // symbol mangling and ':' is its label separator.
    const bool is_method = e.lhs != nullptr;
    std::string class_name;
    if (is_method) {
        const TypePtr& obj_type = e.lhs->type;
        if (obj_type && obj_type->kind == TypeKind::Class) {
            class_name = obj_type->class_name;
        } else if (obj_type && obj_type->kind == TypeKind::Pointer && obj_type->pointee &&
                   obj_type->pointee->kind == TypeKind::Class) {
            class_name = obj_type->pointee->class_name;
        } else {
            fail("cannot work out which class '" + e.name + "' belongs to");
            return;
        }
    }

    // Arguments are evaluated left to right onto the stack, then popped into
    // their ABI registers: register 26 minus the running total of each
    // argument's size rounded up to an even number of bytes.
    std::vector<int> arg_regs;
    int reg = 26;
    if (is_method) reg -= 2;                  // `this` occupies r24:r25
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
        if (expr_size(*a) == 4) { emit("push r22"); emit("push r23"); }
        emit("push r24");
        emit("push r25");
    }

    for (size_t i = e.args.size(); i-- > 0;) {
        int r = arg_regs[i];
        const int bytes = expr_size(*e.args[i]) == 4 ? 4 : 2;
        for (int b = bytes; b-- > 0;) emit("pop r" + imm(r + b));
    }

    if (is_method) {
        // The object's address goes in last, so evaluating it cannot clobber
        // the argument registers already loaded.
        if (e.lhs->type && e.lhs->type->kind == TypeKind::Pointer) {
            gen_expr(*e.lhs);                 // a pointer already *is* the address
        } else {
            gen_address(*this, *e.lhs);
        }
        if (failed()) return;
        emit("call " + class_name + "__" + e.name);
        return;
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

// ------------------------------------------------------------ addresses ----
//
// Subscripts, member access and '&' all need the address of an object rather
// than its value. gen_address() leaves that address in r24:r25; the caller
// then either hands it straight back (for '&') or moves it into Z and reads or
// writes through it. Z is chosen over X because only Y and Z have the
// displacement forms ldd/std, and Y is the frame pointer.

bool gen_address(CodeGen& g, const Expr& e);

bool is_array(const Expr& e) {
    return e.type && e.type->kind == TypeKind::Array;
}

// An expression used as the base of a subscript: an array names its own
// storage, anything else is a pointer whose value is the address.
bool gen_base_pointer(CodeGen& g, const Expr& base) {
    if (is_array(base)) return gen_address(g, base);
    g.gen_expr(base);
    return !g.failed();
}

// Multiplies the index in r24:r25 by the element size. Powers of two shift;
// anything else goes through MUL, which dirties r0 and r1, so r1 is put back.
void scale_index(CodeGen& g, int element) {
    if (element <= 1) return;
    if ((element & (element - 1)) == 0) {
        for (int n = element; n > 1; n >>= 1) {
            g.emit("lsl r24");
            g.emit("rol r25");
        }
        return;
    }
    g.emit("ldi r22, " + imm(element));
    g.emit("mul r24, r22");
    g.emit("movw r18, r0");
    g.emit("mul r25, r22");
    g.emit("add r19, r0");
    g.emit("clr r1");
    g.emit("movw r24, r18");
}

// Adds a constant byte offset to the address in r24:r25.
void add_offset(CodeGen& g, int offset) {
    if (offset == 0) return;
    if (offset > 0 && offset <= 63) { g.emit("adiw r24, " + imm(offset)); return; }
    g.emit("subi r24, " + lo_byte(-offset));
    g.emit("sbci r25, " + hi_byte(-offset));
}

// Reads `size` bytes from the address in r24:r25 back into r24:r25.
void load_through_address(CodeGen& g, int size, bool is_signed) {
    g.emit("movw r30, r24");
    if (size == 4) {
        g.emit("ld r22, Z");
        g.emit("ldd r23, Z+1");
        g.emit("ldd r24, Z+2");
        g.emit("ldd r25, Z+3");
        return;
    }
    g.emit("ld r24, Z");
    if (size == 2) g.emit("ldd r25, Z+1");
    else g.widen_to_16(is_signed);
}

// The class a member expression's object belongs to, or "" if it has none.
std::string member_class(const Expr& e) {
    if (!e.lhs || !e.lhs->type) return {};
    const TypePtr& t = e.lhs->type;
    if (e.through_pointer)
        return t->pointee ? t->pointee->class_name : std::string();
    return t->class_name;
}

bool gen_address(CodeGen& g, const Expr& e) {
    if (g.failed()) return false;

    switch (e.kind) {
    case ExprKind::Identifier: {
        int off = g.local_offset(e.name);
        if (off >= 0) {
            g.emit("movw r24, r28");            // Y, the frame pointer
            add_offset(g, off);
        } else {
            int addr = g.global_address(e.name);
            if (addr < 0) addr = g.add_global(e.name, e.type ? e.type->size() : 2);
            g.emit("ldi r24, " + lo_byte(addr));
            g.emit("ldi r25, " + hi_byte(addr));
        }
        return true;
    }

    case ExprKind::Index: {
        if (!e.lhs || !e.rhs) { g.fail("malformed subscript"); return false; }
        if (!gen_base_pointer(g, *e.lhs)) return false;
        g.emit("push r24");
        g.emit("push r25");
        g.gen_expr(*e.rhs);                     // the subscript
        if (g.failed()) return false;
        scale_index(g, e.type ? e.type->size() : 1);
        g.emit("movw r22, r24");
        g.emit("pop r25");
        g.emit("pop r24");
        g.emit("add r24, r22");
        g.emit("adc r25, r23");
        return true;
    }

    case ExprKind::Member: {
        if (!e.lhs) { g.fail("member access without an object"); return false; }
        std::string cls = member_class(e);
        if (cls.empty()) {
            g.fail("member '" + e.name + "' accessed on a non-class value");
            return false;
        }
        if (e.through_pointer) {
            g.gen_expr(*e.lhs);                 // the pointer is the address
            if (g.failed()) return false;
        } else if (!gen_address(g, *e.lhs)) {
            return false;
        }
        int offset = class_field_offset(cls, e.name);
        if (offset < 0) {
            g.fail("class '" + cls + "' has no field '" + e.name +
                   "' in the generator's layout table");
            return false;
        }
        add_offset(g, offset);
        return true;
    }

    case ExprKind::Unary:
        if (e.op == "*") {
            if (!e.lhs) { g.fail("'*' without an operand"); return false; }
            g.gen_expr(*e.lhs);                 // the pointer value is the address
            return !g.failed();
        }
        break;

    default:
        break;
    }

    g.fail("this expression does not have an address");
    return false;
}

// Reads an lvalue that is not a plain named variable.
void gen_indirect_load(CodeGen& g, const Expr& e) {
    if (is_array(e)) { gen_address(g, e); return; }   // an array decays
    int size = CodeGen::expr_size(e);
    if (e.type && e.type->size() != size) {
        g.fail("only 8-, 16- and 32-bit values can be loaded indirectly");
        return;
    }
    if (!gen_address(g, e)) return;
    load_through_address(g, size, CodeGen::expr_is_signed(e));
}

// -------------------------------------------------------------- strings ----
//
// The bytes of a string literal cannot simply be laid down in flash after the
// code: reading them back would need LPM, and the labels the assembler hands
// out are word addresses while LPM wants a byte address, so a flash string
// could not be handed to a routine that reads it with a plain LD. There is
// also no linker and no startup copy loop to move a .rodata image into SRAM.
//
// So a literal gets a fixed SRAM block, allocated once per distinct string,
// and the code that evaluates the literal fills that block before yielding its
// address. The cost is two instructions per character at each evaluation site;
// the gain is that the result is an ordinary data pointer that every existing
// runtime routine can already read.

// A globals-table key that is unique to the contents, so equal literals share
// one block. The name never reaches the assembler.
std::string string_symbol(const std::string& text) {
    static const char digits[] = "0123456789abcdef";
    std::string name = "__str_";
    for (unsigned char c : text) {
        name += digits[c >> 4];
        name += digits[c & 0x0F];
    }
    return name;
}

void gen_string_literal(CodeGen& g, const Expr& e) {
    const std::string& text = e.str_value;
    int addr = g.add_global(string_symbol(text), int(text.size()) + 1);

    g.emit("ldi r30, " + lo_byte(addr));
    g.emit("ldi r31, " + hi_byte(addr));
    long previous = -1;
    for (size_t i = 0; i <= text.size(); ++i) {
        long byte = i == text.size() ? 0 : long(static_cast<unsigned char>(text[i]));
        if (byte != previous) {                 // r18 already holds a run's value
            g.emit("ldi r18, " + imm(byte));
            previous = byte;
        }
        g.emit("st Z+, r18");
    }
    g.emit("ldi r24, " + lo_byte(addr));
    g.emit("ldi r25, " + hi_byte(addr));
}

// Writes r24:r25 (or r24 alone) to the lvalue `target`, whose address is
// computed after the value, so the value is parked on the stack meanwhile.
void gen_indirect_store(CodeGen& g, const Expr& target, int size) {
    push_value(g, size);
    if (!gen_address(g, target)) return;
    g.emit("movw r30, r24");
    pop_value(g, size);
    if (size == 4) {
        g.emit("st Z, r22");
        g.emit("std Z+1, r23");
        g.emit("std Z+2, r24");
        g.emit("std Z+3, r25");
        return;
    }
    g.emit("st Z, r24");
    if (size == 2) g.emit("std Z+1, r25");
}

} // namespace

void CodeGen::gen_expr(const Expr& e) {
    if (failed()) return;

    switch (e.kind) {

    // ---- literals ---------------------------------------------------------
    case ExprKind::IntLiteral: {
        if (expr_size(e) == 4) {
            const long v = e.int_value;
            emit("ldi r22, " + imm(v & 0xFF));
            emit("ldi r23, " + imm((v >> 8) & 0xFF));
            emit("ldi r24, " + imm((v >> 16) & 0xFF));
            emit("ldi r25, " + imm((v >> 24) & 0xFF));
            return;
        }
        emit("ldi r24, " + lo_byte(e.int_value));
        emit("ldi r25, " + hi_byte(e.int_value));
        return;
    }

    // A string literal evaluates to the address of its bytes. There is no
    // linker and no .data copy loop, so the bytes are materialised into a
    // fixed SRAM block by the code that evaluates the literal: the block is
    // allocated once per distinct string, then filled and its address
    // returned. See the note in gen_string_literal().
    case ExprKind::StringLiteral:
        gen_string_literal(*this, e);
        return;

    // ---- names ------------------------------------------------------------
    case ExprKind::Identifier:
        // An array names its storage; using it as a value yields its address.
        if (e.type && e.type->kind == TypeKind::Array) {
            gen_address(*this, e);
            return;
        }
        load_from_variable(e.name, expr_size(e), expr_is_signed(e));
        return;

    // ---- subscripts and members -------------------------------------------
    case ExprKind::Index:
    case ExprKind::Member:
        gen_indirect_load(*this, e);
        return;

    // ---- casts ------------------------------------------------------------
    case ExprKind::Cast: {
        if (!e.lhs) { fail("cast without an operand"); return; }
        gen_expr(*e.lhs);
        if (failed()) return;
        // Widening sign- or zero-extends according to the *source* type;
        // narrowing keeps the low bytes and re-extends an 8-bit result.
        gen_convert(*this, expr_size(*e.lhs), expr_size(e),
                    expr_is_signed(*e.lhs), expr_is_signed(e));
        return;
    }

    // ---- calls ------------------------------------------------------------
    case ExprKind::Call:
        gen_call(e);
        return;

    // ---- assignment -------------------------------------------------------
    case ExprKind::Assign: {
        if (!e.lhs || !e.rhs) { fail("malformed assignment"); return; }
        const Expr& target = *e.lhs;
        const bool named = target.kind == ExprKind::Identifier;
        const bool indirect = target.kind == ExprKind::Index ||
                              target.kind == ExprKind::Member ||
                              (target.kind == ExprKind::Unary && target.op == "*");
        if (!named && !indirect) {
            fail("this expression cannot be assigned to");
            return;
        }
        int size = expr_size(target);
        if (e.op.empty() || e.op == "=") {
            gen_expr(*e.rhs);
            if (failed()) return;
            // An assignment converts, it does not truncate silently: a long
            // stored into an int keeps the low half, an int stored into a long
            // is extended according to the int's own signedness.
            if (size == 4 || expr_size(*e.rhs) == 4)
                gen_convert(*this, expr_size(*e.rhs), size,
                            expr_is_signed(*e.rhs), expr_is_signed(target));
        } else {
            // Compound assignment: run the plain binary operation with the
            // destination as its left operand.
            gen_binary(e.op.substr(0, e.op.size() - 1), target, *e.rhs, size,
                       expr_is_signed(target));
            if (failed()) return;
        }
        if (named) store_to_variable(target.name, size);
        else gen_indirect_store(*this, target, size);
        return;
    }

    // ---- unary ------------------------------------------------------------
    case ExprKind::Unary: {
        if (!e.lhs) { fail("unary operator without an operand"); return; }
        const std::string& op = e.op;

        // '&' wants the operand's address, never its value.
        if (op == "&") {
            gen_address(*this, *e.lhs);
            return;
        }
        // '*' reads through the pointer the operand evaluates to.
        if (op == "*") {
            gen_indirect_load(*this, e);
            return;
        }

        if (op == "++" || op == "--") {
            if (e.lhs->kind != ExprKind::Identifier) {
                fail("only simple variables can be incremented so far");
                return;
            }
            int size = expr_size(*e.lhs);
            load_from_variable(e.lhs->name, size, expr_is_signed(*e.lhs));
            if (failed()) return;
            if (e.is_postfix) push_value(*this, size);
            if (size == 4) {
                step32(*this, op == "++");
            } else if (op == "++") {
                emit("adiw r24, 1");
            } else {
                emit("sbiw r24, 1");
            }
            store_to_variable(e.lhs->name, size);
            if (failed()) return;
            if (e.is_postfix) pop_value(*this, size);
            return;
        }

        gen_expr(*e.lhs);
        if (failed()) return;

        const int usize = expr_size(e);
        if (usize == 4 && expr_size(*e.lhs) != 4 && op != "!")
            gen_convert(*this, expr_size(*e.lhs), 4, expr_is_signed(*e.lhs),
                        expr_is_signed(e));

        if (op == "+") return;
        if (op == "-") {
            if (usize == 4) {
                negate32(*this);
            } else if (usize == 1) {
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
            if (usize == 4) {
                emit("com r22");
                emit("com r23");
            }
            emit("com r24");
            emit("com r25");
            return;
        }
        if (op == "!") {
            std::string done = new_label("not");
            test_and_consume(*this, expr_size(*e.lhs));
            emit("ldi r24, 0");
            emit("ldi r25, 0");
            emit(std::string("brne ") + done);
            emit("ldi r24, 1");
            emit_label(done);
            if (usize == 4) gen_convert(*this, 2, 4, false, false);
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
        test_and_consume(*this, expr_size(*e.lhs));
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

namespace {

// One 32-bit binary operation. Both operands are evaluated and widened to four
// bytes: the left is parked on the stack while the right is evaluated, then
// moved into r18..r21 so the left can come back to its ABI home in r22..r25.
void gen_wide_binary(CodeGen& g, const std::string& op, const Expr& lhs,
                     const Expr& rhs, int size, bool result_signed) {
    const int lhs_size = CodeGen::expr_size(lhs);
    const int rhs_size = CodeGen::expr_size(rhs);
    const bool lhs_signed = CodeGen::expr_is_signed(lhs);
    const bool rhs_signed = CodeGen::expr_is_signed(rhs);

    g.gen_expr(lhs);
    if (g.failed()) return;
    gen_convert(g, lhs_size, 4, lhs_signed, lhs_signed);
    push_value(g, 4);
    g.gen_expr(rhs);
    if (g.failed()) return;
    gen_convert(g, rhs_size, 4, rhs_signed, rhs_signed);
    g.emit("movw r18, r22");
    g.emit("movw r20, r24");
    pop_value(g, 4);

    // The operands' own signedness decides how the operation behaves; the
    // result type only decides how wide the answer is left.
    const bool is_signed = lhs_signed && rhs_signed;

    auto finish = [&]() {
        if (size != 4) gen_convert(g, 4, size, result_signed, result_signed);
    };

    if (op == "+" || op == "-") {
        const char* first = op == "+" ? "add" : "sub";
        const char* rest = op == "+" ? "adc" : "sbc";
        g.emit(std::string(first) + " r22, r18");
        g.emit(std::string(rest) + " r23, r19");
        g.emit(std::string(rest) + " r24, r20");
        g.emit(std::string(rest) + " r25, r21");
        finish();
        return;
    }
    if (op == "&" || op == "|" || op == "^") {
        const char* m = op == "&" ? "and" : (op == "|" ? "or" : "eor");
        g.emit(std::string(m) + " r22, r18");
        g.emit(std::string(m) + " r23, r19");
        g.emit(std::string(m) + " r24, r20");
        g.emit(std::string(m) + " r25, r21");
        finish();
        return;
    }
    if (op == "*") {
        g.emit("call __ardio_mul32");
        finish();
        return;
    }
    if (op == "/" || op == "%") {
        // The helpers leave the quotient in r22..r25 and the remainder in
        // r26:r27:r30:r31 -- the four call-clobbered registers the dividend and
        // divisor do not occupy. See runtime/math32.S.
        g.emit(is_signed ? "call __ardio_divmod32" : "call __ardio_udivmod32");
        if (op == "%") {
            g.emit("movw r22, r26");
            g.emit("movw r24, r30");
        }
        finish();
        return;
    }
    if (op == "<<" || op == ">>") {
        // AVR shifts one bit at a time, so the count -- the low byte of the
        // right operand -- drives a loop.
        std::string top = g.new_label("shift32");
        std::string done = g.new_label("shift32end");
        g.emit_label(top);
        g.emit("tst r18");
        g.emit("breq " + done);
        if (op == "<<") {
            g.emit("lsl r22");
            g.emit("rol r23");
            g.emit("rol r24");
            g.emit("rol r25");
        } else {
            g.emit(is_signed ? "asr r25" : "lsr r25");
            g.emit("ror r24");
            g.emit("ror r23");
            g.emit("ror r22");
        }
        g.emit("dec r18");
        g.emit("rjmp " + top);
        g.emit_label(done);
        finish();
        return;
    }
    if (is_comparison(op)) {
        Compare c = comparison_branch(op, is_signed);
        std::string done = g.new_label("cmp32");
        if (c.swap) {
            g.emit("cp r18, r22");
            g.emit("cpc r19, r23");
            g.emit("cpc r20, r24");
            g.emit("cpc r21, r25");
        } else {
            g.emit("cp r22, r18");
            g.emit("cpc r23, r19");
            g.emit("cpc r24, r20");
            g.emit("cpc r25, r21");
        }
        g.emit("ldi r24, 1");
        g.emit("ldi r25, 0");
        g.emit(std::string(c.branch) + " " + done);
        g.emit("ldi r24, 0");
        g.emit_label(done);
        if (size == 4) gen_convert(g, 2, 4, false, false);
        return;
    }

    g.fail("unsupported 32-bit binary operator '" + op + "'");
}

} // namespace

void CodeGen::gen_binary(const std::string& op, const Expr& lhs, const Expr& rhs,
                         int size, bool result_signed) {
    if (failed()) return;
    if (size < 1) size = 1;
    if (size > 2 && size != 4) size = 2;

    const int lhs_size = expr_size(lhs);
    const int rhs_size = expr_size(rhs);

    // Short-circuit operators never evaluate the right side unconditionally.
    if (op == "&&" || op == "||") {
        std::string shortcut = new_label(op == "&&" ? "andfalse" : "ortrue");
        std::string end_label = new_label("logend");
        gen_expr(lhs);
        if (failed()) return;
        test_and_consume(*this, lhs_size);
        emit(std::string(op == "&&" ? "breq " : "brne ") + shortcut);
        gen_expr(rhs);
        if (failed()) return;
        test_and_consume(*this, rhs_size);
        emit(std::string(op == "&&" ? "breq " : "brne ") + shortcut);
        emit(std::string("ldi r24, ") + (op == "&&" ? "1" : "0"));
        emit("ldi r25, 0");
        emit("rjmp " + end_label);
        emit_label(shortcut);
        emit(std::string("ldi r24, ") + (op == "&&" ? "0" : "1"));
        emit("ldi r25, 0");
        emit_label(end_label);
        if (size == 4) gen_convert(*this, 2, 4, false, false);
        return;
    }

    // ---- 32-bit --------------------------------------------------------
    //
    // An operation is done at 32 bits when either operand is a long or when
    // the result is: the narrower operand is widened first, so nothing is
    // computed at a width that could lose the answer. The left operand ends
    // in r22..r25 and the right in r18..r21; both halves are call-clobbered,
    // which is what lets the multiply and divide helpers be plain calls.
    const bool comparison = is_comparison(op);
    const bool wide = lhs_size == 4 || rhs_size == 4 ||
                      (!comparison && size == 4);
    if (wide) {
        gen_wide_binary(*this, op, lhs, rhs, size, result_signed);
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
    if (op == "/" || op == "%") {
        // The AVR has no divide instruction, so both operators call the
        // restoring shift-subtract routine in runtime/math.S. It takes the
        // dividend in r24:r25 and the divisor in r22:r23 -- exactly where the
        // operands already are -- and returns the quotient in r24:r25 and the
        // remainder in r18:r19. Everything it touches is call-clobbered.
        const bool operands_signed = expr_is_signed(lhs) && expr_is_signed(rhs);
        emit(operands_signed ? "call __ardio_divmod16" : "call __ardio_udivmod16");
        if (op == "%") emit("movw r24, r18");
        if (size == 1) widen_to_16(is_signed);
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
