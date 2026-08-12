// Class and object code generation for the AVR back end.
//
// A method is an ordinary function with one extra, implicit first argument:
// the address of the object it was called on. That follows the avr-gcc ABI, so
// `this` arrives in r24:r25 and every declared parameter shifts down by one
// slot -- the first declared int lands in r22:r23, the next in r20:r21, and so
// on. The prologue spills `this` into the frame exactly like a real parameter,
// and field accesses are then loads and stores through Z at `this + offset`.
//
// Labels are `ClassName__methodName`, with two underscores: the assembler has
// no symbol mangling and treats ':' as the label separator, so "::" can never
// appear in generated assembly. Constructors are emitted as `ClassName__ctor`.
//
// The expression generator in codegen_expr.cpp knows nothing about classes, so
// this file lowers the class-aware parts of a method body away before handing
// what remains to it. Every field read, method call, field assignment and field
// increment is generated here, its result parked in a hidden frame temporary,
// and the node replaced by a reference to that temporary. What is left is a
// plain expression over locals, which gen_expr() compiles exactly as usual.
//
// Statements are generated here rather than delegated to gen_stmt(), because a
// method needs its own epilogue label and its own break/continue targets, and
// those live in file-local state belonging to the statement generator.

#include "ardio/avr/codegen.h"

#include <map>
#include <string>
#include <vector>

namespace ardio {

// Declared here rather than in codegen.h so that the class back end stays a
// self-contained unit; the driver declares the same prototype to call it.
void gen_global_object_init(CodeGen& g, const Program& p);

// The assembly label for a method or constructor of a class.
std::string class_method_label(const std::string& class_name, const Function& f);

namespace {

// ------------------------------------------------------------- registry ----

// Field offsets of a class other than the one being compiled are needed when a
// method touches a second object (`other.count`). Semantic analysis already
// laid every class out, so the declarations are simply remembered here as they
// pass through, and looked up by name later.
std::map<std::string, const ClassDecl*>& class_registry() {
    static std::map<std::string, const ClassDecl*> classes;
    return classes;
}

void register_class(const ClassDecl& c) { class_registry()[c.name] = &c; }

const ClassDecl* find_class(const std::string& name) {
    auto it = class_registry().find(name);
    return it == class_registry().end() ? nullptr : it->second;
}

const Field* find_field(const ClassDecl& c, const std::string& name) {
    for (const Field& f : c.fields)
        if (f.name == name) return &f;
    return nullptr;
}

const Function* find_constructor(const ClassDecl& c) {
    for (const Function& m : c.methods)
        if (m.is_constructor) return &m;
    return nullptr;
}

// The class a value expression denotes, whether it is an object or a pointer
// to one. Empty when the expression is not class-typed at all.
std::string class_of(const Expr& e) {
    const TypePtr& t = e.type;
    if (!t) return {};
    if (t->kind == TypeKind::Class) return t->class_name;
    if (t->kind == TypeKind::Pointer && t->pointee &&
        t->pointee->kind == TypeKind::Class)
        return t->pointee->class_name;
    return {};
}

bool is_object_value(const Expr& e) {
    return e.type && e.type->kind == TypeKind::Class;
}

// ---------------------------------------------------------------- clone ----

// Method bodies are lowered by rewriting, and the AST handed in is const, so
// each expression is copied before it is touched. Types are shared_ptr and are
// shared rather than duplicated.
ExprPtr clone_expr(const Expr& e) {
    auto copy = std::make_unique<Expr>();
    copy->kind = e.kind;
    copy->line = e.line;
    copy->type = e.type;
    copy->int_value = e.int_value;
    copy->str_value = e.str_value;
    copy->name = e.name;
    copy->op = e.op;
    copy->is_postfix = e.is_postfix;
    copy->through_pointer = e.through_pointer;
    if (e.lhs) copy->lhs = clone_expr(*e.lhs);
    if (e.rhs) copy->rhs = clone_expr(*e.rhs);
    if (e.third) copy->third = clone_expr(*e.third);
    for (const ExprPtr& a : e.args)
        copy->args.push_back(a ? clone_expr(*a) : nullptr);
    return copy;
}

// ---------------------------------------------------------- method state ---

// Everything one method body needs while it is being generated.
struct MethodGen {
    CodeGen& g;
    const ClassDecl& cls;
    int this_offset = 1;
    std::vector<int> temp_offsets;      // frame slots for hoisted values
    size_t next_temp = 0;
    std::string epilogue;
    std::vector<std::pair<std::string, std::string>> loops;  // continue, break

    MethodGen(CodeGen& gen, const ClassDecl& c) : g(gen), cls(c) {}

    void emit(const std::string& line) { g.emit(line); }
    bool failed() const { return g.failed(); }
    void fail(const std::string& m) { g.fail(m); }

    // ---- field and object addressing -------------------------------------

    // A bare name inside a method is a field only when nothing nearer -- a
    // parameter, a local, or a hidden temporary -- has claimed it first.
    const Field* field_for_identifier(const Expr& e) const {
        if (e.kind != ExprKind::Identifier) return nullptr;
        if (g.local_offset(e.name) >= 0) return nullptr;
        return find_field(cls, e.name);
    }

    // The field a member access names, together with the class it belongs to.
    const Field* field_for_member(const Expr& e) const {
        if (e.kind != ExprKind::Member || !e.lhs) return nullptr;
        std::string owner = class_of(*e.lhs);
        if (owner.empty()) return nullptr;
        const ClassDecl* c = find_class(owner);
        if (!c) return nullptr;
        return find_field(*c, e.name);
    }

    const Field* field_ref(const Expr& e) const {
        const Field* f = field_for_identifier(e);
        return f ? f : field_for_member(e);
    }

    // Adds a constant to Z. Small adjustments use adiw; anything larger goes
    // through subi/sbci with the negated constant, which is the only wide add
    // the instruction set offers.
    void add_to_z(int delta) {
        if (delta == 0) return;
        if (delta > 0 && delta <= 63) {
            emit("    adiw r30, " + std::to_string(delta));
            return;
        }
        unsigned neg = unsigned(-delta);
        emit("    subi r30, " + std::to_string(neg & 0xFF));
        emit("    sbci r31, " + std::to_string((neg >> 8) & 0xFF));
    }

    void load_this_into_z() {
        emit("    ldd  r30, Y+" + std::to_string(this_offset));
        emit("    ldd  r31, Y+" + std::to_string(this_offset + 1));
    }

    // Leaves the address of the object an expression denotes in Z.
    void address_into_z(const Expr& e) {
        if (failed()) return;

        if (e.kind == ExprKind::Identifier) {
            // An object of class type: its storage *is* the object.
            if (is_object_value(e)) {
                if (field_for_identifier(e)) {      // a class-typed field of ours
                    load_this_into_z();
                    add_to_z(field_for_identifier(e)->offset);
                    return;
                }
                int off = g.local_offset(e.name);
                if (off >= 0) {
                    emit("    movw r30, r28");
                    add_to_z(off);
                    return;
                }
                int size = e.type ? e.type->size() : 2;
                int addr = g.global_address(e.name);
                if (addr < 0) addr = g.add_global(e.name, size < 1 ? 1 : size);
                emit("    ldi  r30, " + std::to_string(addr & 0xFF));
                emit("    ldi  r31, " + std::to_string((addr >> 8) & 0xFF));
                return;
            }
            // A pointer to an object, `this` included: load the pointer value.
            if (const Field* pf = field_for_identifier(e)) {
                load_this_into_z();
                add_to_z(pf->offset);
                emit("    ld   r0, Z");
                emit("    ldd  r31, Z+1");
                emit("    mov  r30, r0");
                return;
            }
            int off = g.local_offset(e.name);
            if (off >= 0) {
                emit("    ldd  r30, Y+" + std::to_string(off));
                emit("    ldd  r31, Y+" + std::to_string(off + 1));
                return;
            }
            int addr = g.global_address(e.name);
            if (addr < 0) addr = g.add_global(e.name, 2);
            emit("    lds  r30, " + std::to_string(addr));
            emit("    lds  r31, " + std::to_string(addr + 1));
            return;
        }

        if (e.kind == ExprKind::Member && e.lhs) {
            const Field* f = field_for_member(e);
            if (!f) { fail("no such member '" + e.name + "'"); return; }
            address_into_z(*e.lhs);
            if (failed()) return;
            if (is_object_value(e)) { add_to_z(f->offset); return; }
            // A pointer-valued field: read the pointer out of the object.
            add_to_z(f->offset);
            emit("    ld   r0, Z");
            emit("    ldd  r31, Z+1");
            emit("    mov  r30, r0");
            return;
        }

        fail("cannot take the address of this object expression");
    }

    // ---- hidden temporaries ----------------------------------------------

    // Parks r24:r25 in the next free temporary and yields a reference to it.
    ExprPtr park(const TypePtr& type) {
        if (next_temp >= temp_offsets.size()) {
            fail("method needs more temporaries than were reserved");
            return nullptr;
        }
        int off = temp_offsets[next_temp++];
        emit("    std  Y+" + std::to_string(off) + ", r24");
        emit("    std  Y+" + std::to_string(off + 1) + ", r25");
        auto ref = std::make_unique<Expr>();
        ref->kind = ExprKind::Identifier;
        ref->name = ".t" + std::to_string(off);
        ref->type = type;
        return ref;
    }

    // ---- field loads and stores ------------------------------------------

    void load_field(const Expr& e, const Field& f) {
        if (e.kind == ExprKind::Member && e.lhs) address_into_z(*e.lhs);
        else load_this_into_z();
        if (failed()) return;

        int off = f.offset;
        int size = f.type ? f.type->size() : 2;
        if (size < 1 || size > 2) {
            fail("field '" + f.name + "' of class '" + cls.name +
                 "': only 8- and 16-bit fields are supported");
            return;
        }
        if (off > 62) { add_to_z(off); off = 0; }
        emit("    ldd  r24, Z+" + std::to_string(off));
        if (size == 2) emit("    ldd  r25, Z+" + std::to_string(off + 1));
        else g.widen_to_16(f.type ? f.type->is_signed : true);
    }

    // Stores r24:r25 into a field. The object address is recomputed through Z,
    // which is why the value has to be preserved across it.
    void store_field(const Expr& e, const Field& f) {
        int size = f.type ? f.type->size() : 2;
        if (size < 1 || size > 2) {
            fail("field '" + f.name + "' of class '" + cls.name +
                 "': only 8- and 16-bit fields are supported");
            return;
        }
        emit("    push r24");
        emit("    push r25");
        if (e.kind == ExprKind::Member && e.lhs) address_into_z(*e.lhs);
        else load_this_into_z();
        emit("    pop  r25");
        emit("    pop  r24");
        if (failed()) return;

        int off = f.offset;
        if (off > 62) { add_to_z(off); off = 0; }
        emit("    std  Z+" + std::to_string(off) + ", r24");
        if (size == 2) emit("    std  Z+" + std::to_string(off + 1) + ", r25");
    }

    // ---- calls ------------------------------------------------------------

    bool is_method_call(const Expr& e) const {
        if (e.kind != ExprKind::Call) return false;
        if (e.lhs) return true;                       // obj.method(...)
        // An unqualified call inside a method may name a sibling method.
        for (const Function& m : cls.methods)
            if (!m.is_constructor && m.name == e.name) return true;
        return false;
    }

    // Marshals arguments into the ABI registers below `this`, loads the object
    // address into r24:r25, and calls. Arguments are evaluated left to right
    // onto the stack and popped back in reverse, exactly as free calls do.
    void gen_call_with_this(const std::string& label,
                            const std::vector<ExprPtr>& args,
                            const Expr* object) {
        std::vector<int> regs;
        int reg = 26 - 2;                             // `this` takes r24:r25
        for (size_t i = 0; i < args.size(); ++i) {
            reg -= 2;
            if (reg < 8) { fail("too many arguments to '" + label + "'"); return; }
            regs.push_back(reg);
        }

        for (const ExprPtr& a : args) {
            if (!a) { fail("null argument in call to '" + label + "'"); return; }
            gen_value_inline(*a);
            if (failed()) return;
            emit("    push r24");
            emit("    push r25");
        }
        for (size_t i = args.size(); i-- > 0;) {
            emit("    pop  r" + std::to_string(regs[i] + 1));
            emit("    pop  r" + std::to_string(regs[i]));
        }

        if (object) address_into_z(*object);
        else load_this_into_z();
        if (failed()) return;
        emit("    movw r24, r30");
        emit("    call " + label);
    }

    void gen_method_call(const Expr& e) {
        std::string owner = e.lhs ? class_of(*e.lhs) : cls.name;
        if (owner.empty()) { fail("cannot work out the class of a method call"); return; }
        gen_call_with_this(owner + "__" + e.name, e.args, e.lhs.get());
    }

    // ---- lowering ---------------------------------------------------------

    // Rewrites one expression slot, emitting code for every class-aware node
    // it contains and leaving behind a tree gen_expr() can compile on its own.
    void hoist(ExprPtr& slot) {
        if (!slot || failed()) return;
        Expr& e = *slot;

        if (const Field* f = field_ref(e)) {
            load_field(e, *f);
            if (failed()) return;
            ExprPtr ref = park(f->type);
            if (ref) slot = std::move(ref);
            return;
        }

        if (is_method_call(e)) {
            gen_method_call(e);
            if (failed()) return;
            ExprPtr ref = park(e.type);
            if (ref) slot = std::move(ref);
            return;
        }

        if (e.kind == ExprKind::Assign && e.lhs) {
            if (const Field* f = field_ref(*e.lhs)) {
                if (e.op.empty() || e.op == "=") {
                    hoist(e.rhs);
                    if (failed()) return;
                    g.gen_expr(*e.rhs);
                } else {
                    // Compound assignment: read the field, run the plain
                    // operator against the right side, store the result back.
                    ExprPtr lhs_copy = clone_expr(*e.lhs);
                    hoist(lhs_copy);
                    hoist(e.rhs);
                    if (failed()) return;
                    int size = f->type ? f->type->size() : 2;
                    bool sign = f->type ? f->type->is_signed : true;
                    g.gen_binary(e.op.substr(0, e.op.size() - 1), *lhs_copy,
                                 *e.rhs, size, sign);
                }
                if (failed()) return;
                store_field(*e.lhs, *f);
                if (failed()) return;
                ExprPtr ref = park(f->type);
                if (ref) slot = std::move(ref);
                return;
            }
            hoist(e.rhs);                 // a plain local on the left
            return;
        }

        if (e.kind == ExprKind::Unary && (e.op == "++" || e.op == "--") && e.lhs) {
            const Field* f = field_ref(*e.lhs);
            if (!f) return;               // a plain local; gen_expr handles it
            load_field(*e.lhs, *f);
            if (failed()) return;
            ExprPtr before = park(f->type);
            if (!before) return;
            emit(e.op == "++" ? "    adiw r24, 1" : "    sbiw r24, 1");
            store_field(*e.lhs, *f);
            if (failed()) return;
            if (e.is_postfix) {
                slot = std::move(before);          // the value read beforehand
            } else {
                ExprPtr after = park(f->type);
                if (after) slot = std::move(after);
            }
            return;
        }

        hoist(e.lhs);
        hoist(e.rhs);
        hoist(e.third);
        for (ExprPtr& a : e.args) hoist(a);
    }

    // Generates an expression, leaving its value in r24:r25.
    void gen_value_inline(const Expr& e) {
        if (failed()) return;
        ExprPtr copy = clone_expr(e);
        hoist(copy);
        if (failed() || !copy) return;
        g.gen_expr(*copy);
    }

    // A whole expression at statement level: temporaries are recycled between
    // statements, since none of them stay live past one.
    void gen_value(const Expr& e) {
        next_temp = 0;
        gen_value_inline(e);
    }

    // ---- constructors on objects we can address ---------------------------

    void gen_ctor_call(const std::string& class_name,
                       const std::vector<ExprPtr>& args, const Expr& object) {
        const ClassDecl* c = find_class(class_name);
        if (!c) { fail("unknown class '" + class_name + "'"); return; }
        if (!find_constructor(*c)) {
            if (args.empty()) return;    // nothing to run
            fail("class '" + class_name + "' has no constructor to call");
            return;
        }
        gen_call_with_this(class_name + "__ctor", args, &object);
    }

    // ---- statements -------------------------------------------------------

    void gen_stmt(const Stmt& s) {
        if (failed()) return;

        switch (s.kind) {
        case StmtKind::Empty:
            break;

        case StmtKind::Expression:
            if (s.expr) gen_value(*s.expr);
            break;

        case StmtKind::Block:
            for (const StmtPtr& child : s.body)
                if (child) gen_stmt(*child);
            break;

        case StmtKind::VarDecl: {
            next_temp = 0;
            if (s.var_type && s.var_type->kind == TypeKind::Class) {
                if (s.ctor_args.empty()) break;
                Expr object;                      // stands in for the local
                object.kind = ExprKind::Identifier;
                object.name = s.var_name;
                object.type = s.var_type;
                gen_ctor_call(s.var_type->class_name, s.ctor_args, object);
                break;
            }
            if (s.var_init) {
                int size = s.var_type ? s.var_type->size() : 2;
                if (size > 2) {
                    fail("line " + std::to_string(s.line) +
                         ": only 8- and 16-bit locals are supported");
                    return;
                }
                gen_value(*s.var_init);
                if (failed()) return;
                g.store_to_variable(s.var_name, size);
            }
            break;
        }

        case StmtKind::If: {
            std::string else_label = g.new_label("melse");
            std::string end_label = g.new_label("mendif");
            if (s.expr) gen_value(*s.expr);
            emit("    cp   r24, r1");
            emit("    cpc  r25, r1");
            emit("    breq " + (s.else_branch ? else_label : end_label));
            if (s.then_branch) gen_stmt(*s.then_branch);
            if (s.else_branch) {
                emit("    rjmp " + end_label);
                g.emit_label(else_label);
                gen_stmt(*s.else_branch);
            }
            g.emit_label(end_label);
            break;
        }

        case StmtKind::While: {
            std::string top = g.new_label("mwhile");
            std::string done = g.new_label("mendwhile");
            g.emit_label(top);
            if (s.expr) gen_value(*s.expr);
            emit("    cp   r24, r1");
            emit("    cpc  r25, r1");
            emit("    breq " + done);
            loops.emplace_back(top, done);
            if (s.then_branch) gen_stmt(*s.then_branch);
            loops.pop_back();
            emit("    rjmp " + top);
            g.emit_label(done);
            break;
        }

        case StmtKind::For: {
            std::string top = g.new_label("mfor");
            std::string step_label = g.new_label("mforstep");
            std::string done = g.new_label("mendfor");
            if (s.init) gen_stmt(*s.init);
            g.emit_label(top);
            if (s.expr) {
                gen_value(*s.expr);
                emit("    cp   r24, r1");
                emit("    cpc  r25, r1");
                emit("    breq " + done);
            }
            loops.emplace_back(step_label, done);
            if (s.then_branch) gen_stmt(*s.then_branch);
            loops.pop_back();
            g.emit_label(step_label);
            if (s.step) gen_value(*s.step);
            emit("    rjmp " + top);
            g.emit_label(done);
            break;
        }

        case StmtKind::Return:
            if (s.expr) gen_value(*s.expr);
            emit("    rjmp " + epilogue);
            break;

        case StmtKind::Break:
            if (loops.empty()) {
                fail("line " + std::to_string(s.line) + ": break outside a loop");
                return;
            }
            emit("    rjmp " + loops.back().second);
            break;

        case StmtKind::Continue:
            if (loops.empty()) {
                fail("line " + std::to_string(s.line) + ": continue outside a loop");
                return;
            }
            emit("    rjmp " + loops.back().first);
            break;
        }
    }
};

// ------------------------------------------------------- frame planning ----

// Frame slots for every local declared anywhere in the body.
void assign_locals(CodeGen& g, const Stmt& s, int& next) {
    switch (s.kind) {
    case StmtKind::VarDecl: {
        int size = s.var_type ? s.var_type->size() : 2;
        if (size < 1) size = 1;
        g.set_local_offset(s.var_name, next);
        next += size;
        break;
    }
    case StmtKind::Block:
        for (const StmtPtr& c : s.body) if (c) assign_locals(g, *c, next);
        break;
    case StmtKind::If:
        if (s.then_branch) assign_locals(g, *s.then_branch, next);
        if (s.else_branch) assign_locals(g, *s.else_branch, next);
        break;
    case StmtKind::While:
        if (s.then_branch) assign_locals(g, *s.then_branch, next);
        break;
    case StmtKind::For:
        if (s.init) assign_locals(g, *s.init, next);
        if (s.then_branch) assign_locals(g, *s.then_branch, next);
        break;
    default:
        break;
    }
}

// An upper bound on the temporaries one expression needs: every node that
// might be hoisted counts, whether or not it turns out to be one. Counting
// generously costs frame bytes and never correctness.
int count_hoists(const Expr& e) {
    int n = 1;                                   // the node itself, at most one
    if (e.lhs) n += count_hoists(*e.lhs);
    if (e.rhs) n += count_hoists(*e.rhs);
    if (e.third) n += count_hoists(*e.third);
    for (const ExprPtr& a : e.args) if (a) n += count_hoists(*a);
    return n;
}

void max_hoists(const Stmt& s, int& most) {
    auto consider = [&](const Expr* e) {
        if (!e) return;
        int n = count_hoists(*e);
        if (n > most) most = n;
    };
    consider(s.expr.get());
    consider(s.var_init.get());
    consider(s.step.get());
    for (const ExprPtr& a : s.ctor_args) consider(a.get());
    for (const StmtPtr& c : s.body) if (c) max_hoists(*c, most);
    if (s.then_branch) max_hoists(*s.then_branch, most);
    if (s.else_branch) max_hoists(*s.else_branch, most);
    if (s.init) max_hoists(*s.init, most);
}

// The register an implicit-this method's declared parameter arrives in: r24:r25
// belongs to `this`, so allocation starts one slot lower.
int method_argument_register(const std::vector<Param>& params, size_t index) {
    int reg = 26 - 2;
    for (size_t i = 0; i <= index && i < params.size(); ++i) {
        int size = params[i].type ? params[i].type->size() : 2;
        if (size < 1) size = 1;
        reg -= size + (size & 1);
        if (i == index) return reg >= 8 ? reg : -1;
    }
    return -1;
}

void emit_prologue(CodeGen& g, int frame_size) {
    g.emit("    push r28");
    g.emit("    push r29");
    g.emit("    in   r28, 0x3D");               // SPL
    g.emit("    in   r29, 0x3E");               // SPH
    if (frame_size > 0) {
        g.emit("    sbiw r28, " + std::to_string(frame_size));
        g.emit("    in   r0, 0x3F");            // SREG -- the two halves of the
        g.emit("    cli");                      // stack pointer update must not
        g.emit("    out  0x3E, r29");           // be split by an interrupt
        g.emit("    out  0x3F, r0");
        g.emit("    out  0x3D, r28");
    }
}

void emit_epilogue(CodeGen& g, int frame_size) {
    if (frame_size > 0) {
        g.emit("    adiw r28, " + std::to_string(frame_size));
        g.emit("    in   r0, 0x3F");
        g.emit("    cli");
        g.emit("    out  0x3E, r29");
        g.emit("    out  0x3F, r0");
        g.emit("    out  0x3D, r28");
    }
    g.emit("    pop  r29");
    g.emit("    pop  r28");
    g.emit("    ret");
}

} // namespace

// ------------------------------------------------------------- labelling ---

std::string class_method_label(const std::string& class_name, const Function& f) {
    return class_name + "__" + (f.is_constructor ? "ctor" : f.name);
}

// ------------------------------------------------------------- methods -----

void CodeGen::gen_class_method(const ClassDecl& c, const Function& f) {
    if (failed()) return;
    if (!f.body) return;                        // declared but not defined

    register_class(c);

    clear_locals();

    MethodGen mg(*this, c);

    // The frame: `this` first, then the declared parameters, then locals, then
    // the hidden temporaries the lowering needs.
    int next = 1;
    mg.this_offset = next;
    set_local_offset("this", next);              // `this` is a real frame slot
    next += 2;
    for (const Param& p : f.params) {
        int size = p.type ? p.type->size() : 2;
        if (size < 1) size = 1;
        if (size > 2) {
            fail("method '" + class_method_label(c.name, f) +
                 "': only 8- and 16-bit parameters are supported");
            return;
        }
        set_local_offset(p.name, next);
        next += size;
    }
    assign_locals(*this, *f.body, next);

    int temps = 0;
    max_hoists(*f.body, temps);
    for (int i = 0; i < temps; ++i) {
        if (next + 1 > 63) break;               // the check below reports it
        mg.temp_offsets.push_back(next);
        set_local_offset(".t" + std::to_string(next), next);
        next += 2;
    }

    frame_size = next - 1;
    if (frame_size > 62) {
        fail("method '" + class_method_label(c.name, f) + "' needs " +
             std::to_string(frame_size) +
             " bytes of frame; the frame pointer reaches only 62");
        return;
    }

    mg.epilogue = new_label("mepilogue");

    emit("");
    emit("; ---- " + c.name + "::" + (f.is_constructor ? "ctor" : f.name) + " ----");
    emit_label(class_method_label(c.name, f));

    emit_prologue(*this, frame_size);

    // Spill `this` and then every declared argument into the frame.
    emit("    std  Y+" + std::to_string(mg.this_offset) + ", r24");
    emit("    std  Y+" + std::to_string(mg.this_offset + 1) + ", r25");
    for (size_t i = 0; i < f.params.size(); ++i) {
        int reg = method_argument_register(f.params, i);
        if (reg < 0) {
            fail("method '" + class_method_label(c.name, f) +
                 "' has too many parameters to pass in registers");
            return;
        }
        int size = f.params[i].type ? f.params[i].type->size() : 2;
        int off = local_offset(f.params[i].name);
        emit("    std  Y+" + std::to_string(off) + ", r" + std::to_string(reg));
        if (size == 2)
            emit("    std  Y+" + std::to_string(off + 1) + ", r" +
                 std::to_string(reg + 1));
    }

    mg.gen_stmt(*f.body);
    if (failed()) return;

    emit_label(mg.epilogue);
    emit_epilogue(*this, frame_size);
}

// --------------------------------------------------- global construction ---

// Runs the constructor of every global object that was declared with
// arguments. Globals live at fixed SRAM addresses, so the object's address is
// a literal pair of immediates rather than anything the linker has to fix up.
void gen_global_object_init(CodeGen& g, const Program& p) {
    for (const ClassDecl& c : p.classes) register_class(c);

    for (const Global& global : p.globals) {
        if (g.failed()) return;
        if (!global.type || global.type->kind != TypeKind::Class) continue;
        if (global.ctor_args.empty()) continue;

        const ClassDecl* c = find_class(global.type->class_name);
        if (!c) {
            g.fail("unknown class '" + global.type->class_name + "' for global '" +
                   global.name + "'");
            return;
        }
        const Function* ctor = find_constructor(*c);
        if (!ctor) {
            g.fail("class '" + c->name + "' has no constructor, but global '" +
                   global.name + "' is declared with constructor arguments");
            return;
        }

        int size = c->size > 0 ? c->size : 1;
        int addr = g.global_address(global.name);
        if (addr < 0) addr = g.add_global(global.name, size);

        // Arguments first, left to right, then the object address last so that
        // nothing an argument evaluates can clobber r24:r25.
        std::vector<int> regs;
        int reg = 26 - 2;
        for (size_t i = 0; i < global.ctor_args.size(); ++i) {
            reg -= 2;
            if (reg < 8) {
                g.fail("too many constructor arguments for global '" + global.name + "'");
                return;
            }
            regs.push_back(reg);
        }
        for (const ExprPtr& a : global.ctor_args) {
            if (!a) { g.fail("null constructor argument for '" + global.name + "'"); return; }
            g.gen_expr(*a);
            if (g.failed()) return;
            g.emit("    push r24");
            g.emit("    push r25");
        }
        for (size_t i = global.ctor_args.size(); i-- > 0;) {
            g.emit("    pop  r" + std::to_string(regs[i] + 1));
            g.emit("    pop  r" + std::to_string(regs[i]));
        }
        g.emit("    ldi  r24, " + std::to_string(addr & 0xFF));
        g.emit("    ldi  r25, " + std::to_string((addr >> 8) & 0xFF));
        g.emit("    call " + c->name + "__ctor");
    }
}

} // namespace ardio
