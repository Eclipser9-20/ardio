// Statement and function code generation.
//
// Locals live in a frame addressed through Y (r28:r29). The prologue pushes
// the caller's Y, points Y at the current stack top, and opens the frame by
// subtracting its size; the epilogue reverses that exactly. Because ldd/std
// reach only 63 bytes past Y, a frame larger than that is rejected rather than
// silently miscompiled.
//
// A frame is not always needed, though, and for a small function it costs far
// more than the body. So the body is generated first, into a buffer, and the
// prologue is chosen afterwards from what the body turned out to contain:
//
//   * A body that never mentions Y gets no frame at all -- no pushes, no stack
//     pointer arithmetic, no epilogue restore.
//   * A function whose only frame slots are its parameters can keep those
//     parameters in registers instead. Reads of a parameter slot are rewritten
//     from `ldd rD, Y+q` to a `mov` (or deleted outright, when the parameter is
//     still sitting in the register it arrived in), and the frame disappears.
//   * When a frame really is needed, only Y is saved, and a small frame is
//     opened with pushes rather than by writing the stack pointer.
//
// Rewriting the body is safe because it is decoded first: every line is parsed
// into a mnemonic and operands, and anything the decoder does not recognise
// makes the whole function fall back to a plain frame. Nothing is guessed.

#include "ardio/avr/codegen.h"

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ardio {

// Defined beside the expression generator: conversion between the 8/16-bit
// home (r24:r25) and the 32-bit one (r22..r25), and the width-aware test that
// sets Z from a value without destroying it.
void gen_convert(CodeGen& g, int from, int to, bool from_signed, bool to_signed);
void gen_test_value(CodeGen& g, int size);

namespace {

// The width and signedness of the function currently being generated, so a
// `return` can convert its value to the declared return type rather than
// handing back whatever width the expression happened to have.
int& current_return_size() { static int size = 2; return size; }
bool& current_return_signed() { static bool is_signed = true; return is_signed; }

// Breaks and continues need to know where the enclosing loop starts and ends.
struct LoopLabels {
    std::string continue_to;
    std::string break_to;
};

std::vector<LoopLabels>& loop_stack() {
    static std::vector<LoopLabels> stack;
    return stack;
}

// Each function needs its own epilogue label, since `return` jumps to it and
// the assembler rejects duplicates.
std::string& current_epilogue() {
    static std::string label;
    return label;
}

// The register width a declared type occupies, matching CodeGen::expr_size():
// a long is four bytes, everything else is one or two.
int type_width(const TypePtr& t) {
    if (!t) return 2;
    if (t->kind == TypeKind::Long || t->kind == TypeKind::ULong) return 4;
    int n = t->size();
    return n < 1 ? 1 : (n > 2 ? 2 : n);
}

// Walks a statement tree assigning frame slots to every local declaration.
// `wide` comes back true if any of them is 32 bits: a long occupies four
// registers, which is more than parking it in registers is worth, so those
// functions keep a frame.
void assign_local_slots(CodeGen& g, const Stmt& s, int& next_offset, bool& wide) {
    switch (s.kind) {
    case StmtKind::VarDecl: {
        int size = s.var_type ? s.var_type->size() : 2;
        if (size < 1) size = 1;
        if (size == 4) wide = true;
        g.set_local_offset(s.var_name, next_offset);
        next_offset += size;
        break;
    }
    case StmtKind::Block:
        for (const StmtPtr& child : s.body)
            if (child) assign_local_slots(g, *child, next_offset, wide);
        break;
    case StmtKind::If:
        if (s.then_branch) assign_local_slots(g, *s.then_branch, next_offset, wide);
        if (s.else_branch) assign_local_slots(g, *s.else_branch, next_offset, wide);
        break;
    case StmtKind::While:
        if (s.then_branch) assign_local_slots(g, *s.then_branch, next_offset, wide);
        break;
    case StmtKind::For:
        if (s.init) assign_local_slots(g, *s.init, next_offset, wide);
        if (s.then_branch) assign_local_slots(g, *s.then_branch, next_offset, wide);
        break;
    default:
        break;
    }
}

// The ABI allocates argument registers from 26 downwards: each argument takes
// its size rounded up to an even number of bytes, and the low byte lands in the
// resulting register number.
int argument_register(const std::vector<Param>& params, size_t index) {
    int reg = 26;
    for (size_t i = 0; i <= index && i < params.size(); ++i) {
        int size = params[i].type ? params[i].type->size() : 2;
        if (size < 1) size = 1;
        int rounded = size + (size & 1);
        reg -= rounded;
        if (i == index) return reg >= 8 ? reg : -1;   // below r8 means "on the stack"
    }
    return -1;
}

// ----------------------------------------------------------- body decoding --

// One decoded line of generated assembly. Labels, comments and blank lines
// carry an empty mnemonic and are passed through untouched.
struct Insn {
    std::string mnemonic;
    std::vector<std::string> operands;
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

Insn decode(const std::string& raw) {
    Insn insn;
    std::string line = raw;
    size_t comment = line.find(';');
    if (comment != std::string::npos) line = line.substr(0, comment);
    line = trim(line);
    if (line.empty() || line.back() == ':') return insn;   // label or nothing

    size_t space = line.find_first_of(" \t");
    insn.mnemonic = line.substr(0, space);
    if (space == std::string::npos) return insn;

    std::string rest = line.substr(space);
    size_t start = 0;
    while (start <= rest.size()) {
        size_t comma = rest.find(',', start);
        std::string piece = comma == std::string::npos ? rest.substr(start)
                                                       : rest.substr(start, comma - start);
        piece = trim(piece);
        if (!piece.empty()) insn.operands.push_back(piece);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return insn;
}

// `rNN` as a register number, or -1.
int reg_number(const std::string& token) {
    if (token.size() < 2 || token[0] != 'r') return -1;
    int value = 0;
    for (size_t i = 1; i < token.size(); ++i) {
        if (token[i] < '0' || token[i] > '9') return -1;
        value = value * 10 + (token[i] - '0');
    }
    return value < 32 ? value : -1;
}

// Which registers an instruction writes. The generator emits a small, fixed
// vocabulary; anything outside it is reported as unknown so the caller can give
// up rather than assume.
enum class Effect { None, First, Pair, Multiply, Call, Unknown };

Effect effect_of(const std::string& m) {
    static const std::set<std::string> writes_none = {
        "cp", "cpc", "cpi", "cpse", "st", "std", "sts", "out", "push",
        "ret", "reti", "cli", "sei", "nop", "tst", "sbrc", "sbrs",
        "sbic", "sbis", "sec", "clc", "clt", "set", "bst", "rjmp", "jmp",
    };
    static const std::set<std::string> writes_first = {
        "ldi", "ldd", "lds", "ld", "in", "pop", "mov", "com", "clr", "neg",
        "inc", "dec", "lsl", "lsr", "rol", "ror", "asr", "swap", "add", "adc",
        "sub", "subi", "sbc", "sbci", "and", "andi", "or", "ori", "eor",
        "ser", "bld", "cbr", "sbr",
    };
    static const std::set<std::string> writes_pair = {"movw", "adiw", "sbiw"};
    static const std::set<std::string> multiplies = {
        "mul", "muls", "mulsu", "fmul", "fmuls", "fmulsu"};
    static const std::set<std::string> calls = {"call", "rcall", "icall"};

    if (m.rfind("br", 0) == 0) return Effect::None;          // every branch
    if (writes_none.count(m)) return Effect::None;
    if (writes_first.count(m)) return Effect::First;
    if (writes_pair.count(m)) return Effect::Pair;
    if (multiplies.count(m)) return Effect::Multiply;
    if (calls.count(m)) return Effect::Call;
    return Effect::Unknown;
}

// True if the operand names Y, in any of the forms the generator writes.
bool mentions_y(const std::string& operand) {
    if (operand == "Y" || operand == "Y+" || operand == "-Y") return true;
    if (operand.rfind("Y+", 0) == 0) return true;
    int r = reg_number(operand);
    return r == 28 || r == 29;
}

// A parsed `ldd rD, Y+q` or `std Y+q, rS`: the only shapes allowed to touch the
// frame in a function that wants to lose it. Anything else -- taking a slot's
// address, indexing an array through Y -- means the slot must stay in memory.
struct FrameAccess {
    size_t line = 0;
    int reg = 0;                // the register loaded, or the one stored
    int offset = 0;             // displacement from Y
    bool is_write = false;
};

// What a generated body does, as far as the frame decision cares.
struct BodyFacts {
    bool decodable = true;      // every mnemonic was recognised
    bool calls = false;         // contains call/rcall/icall
    bool other_y_use = false;   // touches Y other than through a slot access
    std::vector<FrameAccess> accesses;
    std::set<int> written;      // registers written, ignoring frame reads
    std::set<int> mentioned;    // registers named at all
};

BodyFacts study(const std::vector<std::string>& lines) {
    BodyFacts facts;
    for (size_t i = 0; i < lines.size(); ++i) {
        Insn insn = decode(lines[i]);
        if (insn.mnemonic.empty()) continue;

        Effect effect = effect_of(insn.mnemonic);
        if (effect == Effect::Unknown) { facts.decodable = false; return facts; }
        if (effect == Effect::Call) facts.calls = true;

        bool y_operand = false;
        for (const std::string& operand : insn.operands) {
            if (mentions_y(operand)) y_operand = true;
            int r = reg_number(operand);
            if (r >= 0) facts.mentioned.insert(r);
        }

        // Slot accesses are recorded rather than counted as register writes:
        // the rewriter turns a load into a `mov` from a register, or removes it
        // entirely, so whether it really writes rD depends on the plan.
        if (y_operand && insn.operands.size() == 2) {
            bool load = insn.mnemonic == "ldd" && insn.operands[1].rfind("Y+", 0) == 0;
            bool store = insn.mnemonic == "std" && insn.operands[0].rfind("Y+", 0) == 0;
            if (load || store) {
                const std::string& slot = load ? insn.operands[1] : insn.operands[0];
                int reg = reg_number(load ? insn.operands[0] : insn.operands[1]);
                int offset = std::atoi(slot.c_str() + 2);
                if (reg >= 0) {
                    facts.accesses.push_back({i, reg, offset, store});
                    continue;
                }
            }
        }
        if (y_operand) { facts.other_y_use = true; continue; }

        switch (effect) {
        case Effect::First:
            if (!insn.operands.empty()) {
                int r = reg_number(insn.operands[0]);
                if (r >= 0) facts.written.insert(r);
            }
            break;
        case Effect::Pair:
            if (!insn.operands.empty()) {
                int r = reg_number(insn.operands[0]);
                if (r >= 0) { facts.written.insert(r); facts.written.insert(r + 1); }
            }
            break;
        case Effect::Multiply:
            facts.written.insert(0);
            facts.written.insert(1);
            break;
        case Effect::Call:
            // Call-clobbered by the ABI.
            for (int r = 18; r <= 27; ++r) facts.written.insert(r);
            facts.written.insert(0);
            facts.written.insert(30);
            facts.written.insert(31);
            break;
        default:
            break;
        }
    }
    return facts;
}

// ------------------------------------------------------------ frame plans --

// Where one byte of the frame lives once the frame is gone.
struct Slot {
    int arrives_in = -1;    // the register it arrives in, for a parameter byte
    int home = -1;          // the register the body uses instead of the slot
};

struct RegisterPlan {
    bool usable = false;
    std::map<int, Slot> slots;                   // frame offset -> holder
    std::vector<int> pushes;                     // call-saved registers to save
    std::vector<std::pair<int, int>> copies;     // (home, arrival) copies at entry
};

// The call-saved registers a slot may be parked in. r2-r15 are call-saved by
// the ABI and are touched by neither the expression generator nor the assembly
// runtime, so a value parked there survives anything the body does, calls
// included -- it only has to be pushed and popped like any call-saved register.
// r16 and r17 are deliberately left out: the runtime uses them as scratch
// (saving and restoring them properly), so they are less obviously free, and
// there is no shortage of the others.
constexpr int kFirstHome = 2;
constexpr int kLastHome = 15;

// Parking a byte costs a push and a pop -- four bytes of flash -- against the
// couple of dozen a frame costs however it is opened. The ceiling is well
// inside the fourteen registers available; it is there so that a function with
// a lot of live bytes keeps the frame rather than turning the prologue into a
// long run of pushes.
constexpr size_t kMaxHomedBytes = 10;

// Decides whether the frame slots can live in registers instead. `params` maps
// each parameter byte's frame offset to the register it arrives in; any other
// offset the body touches belongs to a local, which arrives in nothing.
RegisterPlan plan_registers(const std::map<int, int>& params, int frame_size,
                            const BodyFacts& facts) {
    RegisterPlan plan;
    if (!facts.decodable || facts.other_y_use) return plan;

    for (const FrameAccess& access : facts.accesses)
        if (access.offset < 1 || access.offset > frame_size)
            return plan;                        // outside the frame we laid out

    // Which slots are ever written. A parameter that is assigned to cannot stay
    // in the register it arrived in, because the assignment has to land
    // somewhere that later reads will see.
    std::set<int> assigned;
    for (const FrameAccess& access : facts.accesses)
        if (access.is_write) assigned.insert(access.offset);

    // A parameter byte can stay in the register it arrived in when nothing in
    // the body writes that register. Loading the byte into its own register is
    // a no-op that the rewriter deletes, so it does not count as a write -- but
    // loading it anywhere else does, which can disqualify a *different* byte.
    // Start optimistic and retract until the answer stops changing.
    std::map<int, bool> in_place;
    for (const auto& param : params)
        in_place[param.first] = !facts.calls &&        // a call clobbers r18-r27
                                !assigned.count(param.first);

    for (bool changed = true; changed;) {
        changed = false;
        std::set<int> written = facts.written;
        for (const FrameAccess& access : facts.accesses) {
            if (access.is_write) continue;             // becomes a write of the home
            auto param = params.find(access.offset);
            bool deleted = param != params.end() && in_place[access.offset] &&
                           access.reg == param->second;
            if (!deleted) written.insert(access.reg);
        }
        for (const auto& param : params) {
            if (in_place[param.first] && written.count(param.second)) {
                in_place[param.first] = false;
                changed = true;
            }
        }
    }

    // Every slot the body actually touches needs somewhere to live. A parameter
    // that stays put costs nothing; anything else is parked in a call-saved
    // register, and a parameter is copied into it on entry.
    int next_home = kFirstHome;
    for (const FrameAccess& access : facts.accesses) {
        if (plan.slots.count(access.offset)) continue;
        Slot slot;
        auto param = params.find(access.offset);
        if (param != params.end()) slot.arrives_in = param->second;
        if (param != params.end() && in_place[access.offset]) {
            slot.home = param->second;
        } else {
            if (next_home > kLastHome) return plan;
            // The body must not touch the register being parked in.
            if (facts.mentioned.count(next_home)) return plan;
            slot.home = next_home++;
            plan.pushes.push_back(slot.home);
            if (slot.arrives_in >= 0) plan.copies.push_back({slot.home, slot.arrives_in});
        }
        plan.slots[access.offset] = slot;
    }
    if (plan.pushes.size() > kMaxHomedBytes) return plan;

    plan.usable = true;
    return plan;
}

// Applies a plan to the generated body: a slot load becomes a register move, or
// vanishes when the value is already in the right register, and a slot store
// becomes a move into the slot's home.
std::vector<std::string> rewrite(const std::vector<std::string>& lines,
                                 const BodyFacts& facts, const RegisterPlan& plan) {
    std::map<size_t, std::string> replacement;
    for (const FrameAccess& access : facts.accesses) {
        int home = plan.slots.at(access.offset).home;
        int from = access.is_write ? access.reg : home;
        int to = access.is_write ? home : access.reg;
        if (from == to)
            replacement[access.line] = "";             // already there
        else
            replacement[access.line] = "    mov  r" + std::to_string(to) +
                                       ", r" + std::to_string(from);
    }

    std::vector<std::string> result;
    result.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        auto it = replacement.find(i);
        if (it == replacement.end()) { result.push_back(lines[i]); continue; }
        if (!it->second.empty()) result.push_back(it->second);
    }
    return result;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) { lines.push_back(text.substr(start)); break; }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

// Opening a frame by writing SPH and SPL is two non-atomic halves, so an
// interrupt taken between them would see -- and push onto -- a half-updated
// stack pointer. That is why the long form disables interrupts across the pair.
// A small frame avoids the question entirely by pushing its bytes instead: each
// push moves the stack pointer by one byte atomically, so there is no window to
// protect, and Y is then simply read back from the settled pointer. Below six
// bytes that is also the shorter sequence, which is why the threshold sits
// where it does rather than being pushed higher.
constexpr int kPushFrameLimit = 5;

} // namespace

void CodeGen::gen_stmt(const Stmt& s) {
    if (failed()) return;

    switch (s.kind) {
    case StmtKind::Empty:
        break;

    case StmtKind::Expression:
        if (s.expr) gen_expr(*s.expr);
        break;

    case StmtKind::Block:
        for (const StmtPtr& child : s.body)
            if (child) gen_stmt(*child);
        break;

    case StmtKind::VarDecl: {
        // Storage was reserved when the frame was laid out; only the
        // initialiser needs code here.
        if (s.var_init) {
            gen_expr(*s.var_init);
            if (failed()) return;
            int size = type_width(s.var_type);
            int declared = s.var_type ? s.var_type->size() : 2;
            if (declared != size) {
                fail("line " + std::to_string(s.line) +
                     ": only 8-, 16- and 32-bit locals are supported");
                return;
            }
            int from = expr_size(*s.var_init);
            if (from == 4 || size == 4)
                gen_convert(*this, from, size, expr_is_signed(*s.var_init),
                            s.var_type ? s.var_type->is_signed : true);
            store_to_variable(s.var_name, size);
        }
        break;
    }

    case StmtKind::If: {
        std::string else_label = new_label("else");
        std::string end_label = new_label("endif");
        // A value of zero is false; anything else is true.
        if (s.expr) { gen_expr(*s.expr); gen_test_value(*this, expr_size(*s.expr)); }
        else gen_test_value(*this, 2);
        emit("    breq " + (s.else_branch ? else_label : end_label));
        if (s.then_branch) gen_stmt(*s.then_branch);
        if (s.else_branch) {
            emit("    rjmp " + end_label);
            emit_label(else_label);
            gen_stmt(*s.else_branch);
        }
        emit_label(end_label);
        break;
    }

    case StmtKind::While: {
        std::string top = new_label("while");
        std::string done = new_label("endwhile");
        emit_label(top);
        if (s.expr) { gen_expr(*s.expr); gen_test_value(*this, expr_size(*s.expr)); }
        else gen_test_value(*this, 2);
        emit("    breq " + done);
        loop_stack().push_back({top, done});
        if (s.then_branch) gen_stmt(*s.then_branch);
        loop_stack().pop_back();
        emit("    rjmp " + top);
        emit_label(done);
        break;
    }

    case StmtKind::For: {
        std::string top = new_label("for");
        std::string step_label = new_label("forstep");
        std::string done = new_label("endfor");
        if (s.init) gen_stmt(*s.init);
        emit_label(top);
        if (s.expr) {                       // an absent condition means "true"
            gen_expr(*s.expr);
            gen_test_value(*this, expr_size(*s.expr));
            emit("    breq " + done);
        }
        loop_stack().push_back({step_label, done});
        if (s.then_branch) gen_stmt(*s.then_branch);
        loop_stack().pop_back();
        emit_label(step_label);             // continue lands here, not at the top
        if (s.step) gen_expr(*s.step);
        emit("    rjmp " + top);
        emit_label(done);
        break;
    }

    case StmtKind::Return:
        if (s.expr) {
            gen_expr(*s.expr);                  // value is already in its ABI home
            if (failed()) return;
            int from = expr_size(*s.expr);
            if (from == 4 || current_return_size() == 4)
                gen_convert(*this, from, current_return_size(),
                            expr_is_signed(*s.expr), current_return_signed());
        }
        emit("    rjmp " + current_epilogue());
        break;

    case StmtKind::Break:
        if (loop_stack().empty()) { fail("line " + std::to_string(s.line) +
                                         ": break outside a loop"); return; }
        emit("    rjmp " + loop_stack().back().break_to);
        break;

    case StmtKind::Continue:
        if (loop_stack().empty()) { fail("line " + std::to_string(s.line) +
                                         ": continue outside a loop"); return; }
        emit("    rjmp " + loop_stack().back().continue_to);
        break;
    }
}

void CodeGen::gen_function(const Function& f) {
    if (failed()) return;
    if (!f.body) return;                    // a declaration with no definition

    clear_locals();
    loop_stack().clear();

    // Lay out the frame: parameters first, then locals.
    int next_offset = 1;                    // ldd/std use Y+1..Y+63
    for (const Param& p : f.params) {
        int size = p.type ? p.type->size() : 2;
        if (size < 1) size = 1;
        set_local_offset(p.name, next_offset);
        next_offset += size;
    }
    bool wide_local = false;
    assign_local_slots(*this, *f.body, next_offset, wide_local);

    frame_size = next_offset - 1;
    if (frame_size > 62) {
        fail("function '" + f.name + "' needs " + std::to_string(frame_size) +
             " bytes of locals; the frame pointer reaches only 62");
        return;
    }

    std::string epilogue = new_label("epilogue");
    current_epilogue() = epilogue;
    current_return_size() = f.return_type ? type_width(f.return_type) : 2;
    current_return_signed() = f.return_type ? f.return_type->is_signed : true;

    // Where every parameter byte arrives, checked before anything is emitted so
    // an unsupported signature is rejected the same way it always was.
    std::map<int, int> param_bytes;             // frame offset -> ABI register
    bool wide_param = false;                    // a 32-bit parameter
    for (size_t i = 0; i < f.params.size(); ++i) {
        int reg = argument_register(f.params, i);
        if (reg < 0) {
            fail("function '" + f.name + "' has too many parameters to pass in registers");
            return;
        }
        int size = f.params[i].type ? f.params[i].type->size() : 2;
        if (size != type_width(f.params[i].type)) {
            fail("function '" + f.name +
                 "': only 8-, 16- and 32-bit parameters are supported");
            return;
        }
        int offset = local_offset(f.params[i].name);
        if (offset + size - 1 > 63) {
            fail("function '" + f.name + "': parameter '" + f.params[i].name +
                 "' lies beyond the frame pointer's reach");
            return;
        }
        // A 32-bit argument arrives in four consecutive registers, low byte
        // first, matching four consecutive frame bytes.
        if (size == 4) wide_param = true;
        for (int b = 0; b < size; ++b) param_bytes[offset + b] = reg + b;
    }

    emit("");
    emit("; ---- " + f.name + " ----");
    emit_label(f.name);

    // The body decides the prologue, so it is generated first and held aside.
    const size_t body_start = out.size();
    gen_stmt(*f.body);
    if (failed()) return;
    std::vector<std::string> body = split_lines(out.substr(body_start));
    out.resize(body_start);

    BodyFacts facts = study(body);

    // A 32-bit value occupies four registers, so parking one ties up four
    // call-saved registers to save a single slot; functions holding one keep
    // their frame.
    RegisterPlan plan;
    if (!wide_param && !wide_local) plan = plan_registers(param_bytes, frame_size, facts);

    if (plan.usable) {
        // No frame: Y is never touched, so it is never saved. Only the
        // call-saved registers actually parked in are pushed, and only a
        // parameter that could not stay where it arrived is copied.
        for (int reg : plan.pushes) emit("    push r" + std::to_string(reg));
        for (const auto& copy : plan.copies)
            emit("    mov  r" + std::to_string(copy.first) + ", r" +
                 std::to_string(copy.second));
        for (const std::string& line : rewrite(body, facts, plan)) emit(line);
        emit_label(epilogue);
        for (size_t i = plan.pushes.size(); i-- > 0;)
            emit("    pop  r" + std::to_string(plan.pushes[i]));
        emit("    ret");
        frame_size = 0;
        return;
    }

    // A frame it is. Y is saved because it is about to be overwritten; nothing
    // else is.
    emit("    push r28");
    emit("    push r29");
    if (frame_size > 0 && frame_size <= kPushFrameLimit) {
        // Push the frame open one byte at a time: each push moves the stack
        // pointer atomically, so no interrupt can observe a half-written
        // pointer and there is nothing to disable interrupts for.
        for (int i = 0; i < frame_size; ++i) emit("    push r1");
        emit("    in   r28, 0x3D");         // SPL -- Y now points just below
        emit("    in   r29, 0x3E");         // SPH    the bytes just pushed
    } else if (frame_size > 0) {
        emit("    in   r28, 0x3D");         // SPL
        emit("    in   r29, 0x3E");         // SPH
        emit("    sbiw r28, " + std::to_string(frame_size));
        emit("    in   r0, 0x3F");          // SREG -- the stack pointer update
        emit("    cli");                    // must not be interrupted between
        emit("    out  0x3E, r29");         // its two halves
        emit("    out  0x3F, r0");
        emit("    out  0x3D, r28");
    } else {
        emit("    in   r28, 0x3D");
        emit("    in   r29, 0x3E");
    }

    // Spill incoming arguments from their ABI registers into the frame.
    for (const auto& byte : param_bytes)
        emit("    std  Y+" + std::to_string(byte.first) + ", r" +
             std::to_string(byte.second));

    for (const std::string& line : body) emit(line);

    emit_label(epilogue);
    if (frame_size > 0 && frame_size <= kPushFrameLimit) {
        for (int i = 0; i < frame_size; ++i) emit("    pop  r0");
    } else if (frame_size > 0) {
        emit("    adiw r28, " + std::to_string(frame_size));
        emit("    in   r0, 0x3F");
        emit("    cli");
        emit("    out  0x3E, r29");
        emit("    out  0x3F, r0");
        emit("    out  0x3D, r28");
    }
    emit("    pop  r29");
    emit("    pop  r28");
    emit("    ret");
}

} // namespace ardio
