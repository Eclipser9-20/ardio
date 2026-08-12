// Statement and function code generation.
//
// Locals live in a frame addressed through Y (r28:r29). The prologue pushes
// the caller's Y, points Y at the current stack top, and opens the frame by
// subtracting its size; the epilogue reverses that exactly. Because ldd/std
// reach only 63 bytes past Y, a frame larger than that is rejected rather than
// silently miscompiled.

#include "ardio/avr/codegen.h"

#include <vector>

namespace ardio {
namespace {

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

// Walks a statement tree assigning frame slots to every local declaration.
void assign_local_slots(CodeGen& g, const Stmt& s, int& next_offset) {
    switch (s.kind) {
    case StmtKind::VarDecl: {
        int size = s.var_type ? s.var_type->size() : 2;
        if (size < 1) size = 1;
        g.set_local_offset(s.var_name, next_offset);
        next_offset += size;
        break;
    }
    case StmtKind::Block:
        for (const StmtPtr& child : s.body)
            if (child) assign_local_slots(g, *child, next_offset);
        break;
    case StmtKind::If:
        if (s.then_branch) assign_local_slots(g, *s.then_branch, next_offset);
        if (s.else_branch) assign_local_slots(g, *s.else_branch, next_offset);
        break;
    case StmtKind::While:
        if (s.then_branch) assign_local_slots(g, *s.then_branch, next_offset);
        break;
    case StmtKind::For:
        if (s.init) assign_local_slots(g, *s.init, next_offset);
        if (s.then_branch) assign_local_slots(g, *s.then_branch, next_offset);
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
            int size = s.var_type ? s.var_type->size() : 2;
            if (size > 2) { fail("line " + std::to_string(s.line) +
                                 ": only 8- and 16-bit locals are supported"); return; }
            store_to_variable(s.var_name, size);
        }
        break;
    }

    case StmtKind::If: {
        std::string else_label = new_label("else");
        std::string end_label = new_label("endif");
        if (s.expr) gen_expr(*s.expr);
        // A value of zero is false; anything else is true.
        emit("    cp   r24, r1");
        emit("    cpc  r25, r1");
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
        if (s.expr) gen_expr(*s.expr);
        emit("    cp   r24, r1");
        emit("    cpc  r25, r1");
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
            emit("    cp   r24, r1");
            emit("    cpc  r25, r1");
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
        if (s.expr) gen_expr(*s.expr);      // value is already in r24:r25
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
    assign_local_slots(*this, *f.body, next_offset);

    frame_size = next_offset - 1;
    if (frame_size > 62) {
        fail("function '" + f.name + "' needs " + std::to_string(frame_size) +
             " bytes of locals; the frame pointer reaches only 62");
        return;
    }

    std::string epilogue = new_label("epilogue");
    current_epilogue() = epilogue;

    emit("");
    emit("; ---- " + f.name + " ----");
    emit_label(f.name);

    // Prologue: save the caller's frame pointer, then open our own frame.
    emit("    push r28");
    emit("    push r29");
    if (frame_size > 0) {
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
    for (size_t i = 0; i < f.params.size(); ++i) {
        int reg = argument_register(f.params, i);
        if (reg < 0) {
            fail("function '" + f.name + "' has too many parameters to pass in registers");
            return;
        }
        int size = f.params[i].type ? f.params[i].type->size() : 2;
        if (size > 2) {
            fail("function '" + f.name + "': only 8- and 16-bit parameters are supported");
            return;
        }
        int offset = local_offset(f.params[i].name);
        emit("    std  Y+" + std::to_string(offset) + ", r" + std::to_string(reg));
        if (size == 2)
            emit("    std  Y+" + std::to_string(offset + 1) + ", r" +
                 std::to_string(reg + 1));
    }

    gen_stmt(*f.body);

    emit_label(epilogue);
    if (frame_size > 0) {
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
