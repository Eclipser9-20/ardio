// Tests for class and object code generation.
//
// Every test builds its AST by hand -- the parser is not involved -- and then
// assembles the generated text with ardio::assemble(), so a test only passes
// when the assembly really encodes to machine code. Checking the text alone
// would happily accept a mnemonic the assembler does not know.

#include "harness.h"

#include "ardio/avr/assembler.h"
#include "ardio/avr/codegen.h"
#include "ardio/avr/sema.h"

#include <string>
#include <utility>
#include <vector>

namespace ardio {
// Declared by the class back end; the driver declares the same prototype.
void gen_global_object_init(CodeGen& g, const Program& p);
}

using namespace ardio;

namespace {

// ------------------------------------------------------- little builders ---

TypePtr int_type() { return make_type(TypeKind::Int); }

TypePtr class_type(const std::string& name) {
    auto t = make_type(TypeKind::Class);
    t->class_name = name;
    return t;
}

ExprPtr num(long v) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::IntLiteral;
    e->int_value = v;
    e->type = int_type();
    return e;
}

ExprPtr ident(const std::string& name, TypePtr type) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Identifier;
    e->name = name;
    e->type = std::move(type);
    return e;
}

ExprPtr binary(const std::string& op, ExprPtr l, ExprPtr r) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Binary;
    e->op = op;
    e->lhs = std::move(l);
    e->rhs = std::move(r);
    e->type = int_type();
    return e;
}

ExprPtr assign(ExprPtr l, ExprPtr r) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Assign;
    e->op = "=";
    e->type = l->type;
    e->lhs = std::move(l);
    e->rhs = std::move(r);
    return e;
}

// A call on an object: `object.name(args...)`, the shape semantic analysis
// leaves behind for a method call.
ExprPtr method_call(ExprPtr object, const std::string& name,
                    std::vector<ExprPtr> args) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Call;
    e->name = name;
    e->lhs = std::move(object);
    e->args = std::move(args);
    e->type = int_type();
    return e;
}

ExprPtr member(ExprPtr object, const std::string& name, TypePtr type,
               bool through_pointer) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Member;
    e->name = name;
    e->lhs = std::move(object);
    e->type = std::move(type);
    e->through_pointer = through_pointer;
    return e;
}

StmtPtr expr_stmt(ExprPtr e) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Expression;
    s->expr = std::move(e);
    return s;
}

StmtPtr return_stmt(ExprPtr e) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Return;
    s->expr = std::move(e);
    return s;
}

StmtPtr block() {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Block;
    return s;
}

void add_to(Stmt& b, StmtPtr s) { b.body.push_back(std::move(s)); }

Function make_method(const std::string& name, const std::string& owner,
                     std::vector<Param> params, StmtPtr body,
                     bool is_ctor = false) {
    Function f;
    f.name = name;
    f.owner_class = owner;
    f.is_constructor = is_ctor;
    f.return_type = int_type();
    f.params = std::move(params);
    f.body = std::move(body);
    return f;
}

// A class of two int fields, laid out the way sema would lay it out.
ClassDecl make_counter() {
    ClassDecl c;
    c.name = "Counter";
    c.fields.push_back({"value", int_type(), 0});
    c.fields.push_back({"step", int_type(), 2});
    c.size = 4;
    set_class_size("Counter", 4);
    return c;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Assembles and reports the assembler's own message when it refuses.
bool assembles(const std::string& text) {
    AssembleResult r = assemble(text);
    if (!r.ok) std::printf("    assembler said: %s\n", r.error.c_str());
    return r.ok && !r.code.empty();
}

} // namespace

// --------------------------------------------------------------- tests -----

TEST(method_label_uses_two_underscores) {
    ClassDecl c = make_counter();
    // void add(int n) { value = value + n; }
    StmtPtr body = block();
    add_to(*body, expr_stmt(assign(ident("value", int_type()),
                                   binary("+", ident("value", int_type()),
                                          ident("n", int_type())))));
    Function f = make_method("add", "Counter", {{"n", int_type()}}, std::move(body));

    CodeGen g;
    g.gen_class_method(c, f);

    CHECK(g.error.empty());
    CHECK(contains(g.out, "Counter__add:"));
    // A label may never contain "::" -- ':' is the assembler's separator.
    for (const std::string& bad : {std::string("Counter::add:")})
        CHECK(!contains(g.out, bad));
    CHECK(assembles(g.out));
}

TEST(this_and_parameters_land_in_the_abi_registers) {
    ClassDecl c = make_counter();
    // void set2(int a, int b) { value = a; step = b; }
    StmtPtr body = block();
    add_to(*body, expr_stmt(assign(ident("value", int_type()), ident("a", int_type()))));
    add_to(*body, expr_stmt(assign(ident("step", int_type()), ident("b", int_type()))));
    Function f = make_method("set2", "Counter",
                             {{"a", int_type()}, {"b", int_type()}}, std::move(body));

    CodeGen g;
    g.gen_class_method(c, f);

    CHECK(g.error.empty());
    // `this` is spilled first, from r24:r25; the declared ints follow in
    // r22:r23 and r20:r21, one slot lower than a free function would use.
    CHECK(contains(g.out, "std  Y+1, r24"));
    CHECK(contains(g.out, "std  Y+2, r25"));
    CHECK(contains(g.out, "std  Y+3, r22"));
    CHECK(contains(g.out, "std  Y+4, r23"));
    CHECK(contains(g.out, "std  Y+5, r20"));
    CHECK(contains(g.out, "std  Y+6, r21"));
    CHECK(assembles(g.out));
}

TEST(fields_are_addressed_at_this_plus_offset) {
    ClassDecl c = make_counter();
    // int get_step() { return step; }   -- 'step' sits at offset 2
    StmtPtr body = block();
    add_to(*body, return_stmt(ident("step", int_type())));
    Function f = make_method("get_step", "Counter", {}, std::move(body));

    CodeGen g;
    g.gen_class_method(c, f);

    CHECK(g.error.empty());
    CHECK(contains(g.out, "ldd  r30, Y+1"));      // this, out of the frame
    CHECK(contains(g.out, "ldd  r31, Y+2"));
    CHECK(contains(g.out, "ldd  r24, Z+2"));      // step, at this + 2
    CHECK(contains(g.out, "ldd  r25, Z+3"));
    CHECK(assembles(g.out));
}

TEST(field_assignment_stores_through_z) {
    ClassDecl c = make_counter();
    // void reset() { value = 0; }   -- 'value' sits at offset 0
    StmtPtr body = block();
    add_to(*body, expr_stmt(assign(ident("value", int_type()), num(0))));
    Function f = make_method("reset", "Counter", {}, std::move(body));

    CodeGen g;
    g.gen_class_method(c, f);

    CHECK(g.error.empty());
    CHECK(contains(g.out, "std  Z+0, r24"));
    CHECK(contains(g.out, "std  Z+1, r25"));
    CHECK(assembles(g.out));
}

TEST(a_parameter_shadows_a_field_of_the_same_name) {
    ClassDecl c = make_counter();
    // int echo(int value) { return value; }  -- the parameter wins
    StmtPtr body = block();
    add_to(*body, return_stmt(ident("value", int_type())));
    Function f = make_method("echo", "Counter", {{"value", int_type()}}, std::move(body));

    CodeGen g;
    g.gen_class_method(c, f);

    CHECK(g.error.empty());
    CHECK(contains(g.out, "ldd r24, Y+3"));      // the parameter slot
    CHECK(!contains(g.out, "ldd  r24, Z+"));     // never the field
    CHECK(assembles(g.out));
}

TEST(this_pointer_member_access_reads_the_same_field) {
    ClassDecl c = make_counter();
    // int get() { return this->value; }
    auto self = make_pointer(class_type("Counter"));
    StmtPtr body = block();
    add_to(*body, return_stmt(member(ident("this", self), "value", int_type(), true)));
    Function f = make_method("get", "Counter", {}, std::move(body));

    CodeGen g;
    g.gen_class_method(c, f);

    CHECK(g.error.empty());
    CHECK(contains(g.out, "ldd  r30, Y+1"));
    CHECK(contains(g.out, "ldd  r24, Z+0"));
    CHECK(assembles(g.out));
}

TEST(constructor_is_emitted_as_ctor) {
    ClassDecl c = make_counter();
    // Counter(int start) { value = start; step = 1; }
    StmtPtr body = block();
    add_to(*body, expr_stmt(assign(ident("value", int_type()), ident("start", int_type()))));
    add_to(*body, expr_stmt(assign(ident("step", int_type()), num(1))));
    Function f = make_method("Counter", "Counter", {{"start", int_type()}},
                             std::move(body), true);

    CodeGen g;
    g.gen_class_method(c, f);

    CHECK(g.error.empty());
    CHECK(contains(g.out, "Counter__ctor:"));
    CHECK(contains(g.out, "std  Y+1, r24"));     // this
    CHECK(contains(g.out, "std  Y+3, r22"));     // start
    CHECK(assembles(g.out));
}

TEST(a_sibling_method_call_passes_this_first) {
    ClassDecl c = make_counter();
    c.methods.push_back(make_method("add", "Counter", {{"n", int_type()}}, nullptr));

    // void bump() { add(step); }
    StmtPtr body = block();
    std::vector<ExprPtr> args;
    args.push_back(ident("step", int_type()));
    auto call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->name = "add";
    call->args = std::move(args);
    call->type = int_type();
    add_to(*body, expr_stmt(std::move(call)));

    Function bump = make_method("bump", "Counter", {}, std::move(body));

    CodeGen g;
    g.gen_class_method(c, bump);

    CHECK(g.error.empty());
    CHECK(contains(g.out, "pop  r22"));          // the declared argument
    CHECK(contains(g.out, "movw r24, r30"));     // this, loaded last
    CHECK(contains(g.out, "call Counter__add"));

    // The callee has no body here, so give the assembler something to reach.
    CHECK(assembles(g.out + "\nCounter__add:\n    ret\n"));
}

TEST(a_call_on_another_object_uses_that_objects_address) {
    ClassDecl c = make_counter();
    c.methods.push_back(make_method("add", "Counter", {{"n", int_type()}}, nullptr));

    // void copy_to(Counter other) { other.add(value); }
    StmtPtr body = block();
    std::vector<ExprPtr> args;
    args.push_back(ident("value", int_type()));
    add_to(*body, expr_stmt(method_call(ident("other", class_type("Counter")),
                                        "add", std::move(args))));
    Function f = make_method("copy_to", "Counter", {}, std::move(body));

    CodeGen g;
    g.add_global("other", 4);
    g.gen_class_method(c, f);

    CHECK(g.error.empty());
    CHECK(contains(g.out, "call Counter__add"));
    CHECK(contains(g.out, "movw r24, r30"));
    CHECK(assembles(g.out + "\nCounter__add:\n    ret\n"));
}

TEST(return_inside_a_method_reaches_its_own_epilogue) {
    ClassDecl c = make_counter();

    StmtPtr first = block();
    add_to(*first, return_stmt(ident("value", int_type())));
    Function a = make_method("get_a", "Counter", {}, std::move(first));

    StmtPtr second = block();
    add_to(*second, return_stmt(ident("step", int_type())));
    Function b = make_method("get_b", "Counter", {}, std::move(second));

    CodeGen g;
    g.gen_class_method(c, a);
    g.gen_class_method(c, b);

    CHECK(g.error.empty());
    // Two methods in one unit means two distinct epilogue labels; a duplicate
    // would be rejected by the assembler rather than silently merged.
    CHECK(assembles(g.out));
}

TEST(compound_assignment_to_a_field_reads_and_writes_it) {
    ClassDecl c = make_counter();
    // void grow(int n) { value += n; }
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Assign;
    e->op = "+=";
    e->type = int_type();
    e->lhs = ident("value", int_type());
    e->rhs = ident("n", int_type());
    StmtPtr body = block();
    add_to(*body, expr_stmt(std::move(e)));
    Function f = make_method("grow", "Counter", {{"n", int_type()}}, std::move(body));

    CodeGen g;
    g.gen_class_method(c, f);

    CHECK(g.error.empty());
    CHECK(contains(g.out, "ldd  r24, Z+0"));     // read the field
    CHECK(contains(g.out, "add r24, r22"));      // the operation itself
    CHECK(contains(g.out, "std  Z+0, r24"));     // write it back
    CHECK(assembles(g.out));
}

TEST(incrementing_a_field_is_a_read_modify_write) {
    ClassDecl c = make_counter();
    // void tick() { value++; }
    auto inc = std::make_unique<Expr>();
    inc->kind = ExprKind::Unary;
    inc->op = "++";
    inc->is_postfix = true;
    inc->type = int_type();
    inc->lhs = ident("value", int_type());
    StmtPtr body = block();
    add_to(*body, expr_stmt(std::move(inc)));
    Function f = make_method("tick", "Counter", {}, std::move(body));

    CodeGen g;
    g.gen_class_method(c, f);

    CHECK(g.error.empty());
    CHECK(contains(g.out, "ldd  r24, Z+0"));
    CHECK(contains(g.out, "adiw r24, 1"));
    CHECK(contains(g.out, "std  Z+0, r24"));
    CHECK(assembles(g.out));
}

TEST(a_loop_over_a_field_still_assembles) {
    ClassDecl c = make_counter();
    // void drain() { while (value) { value = value - step; } }
    auto body_stmt = block();
    add_to(*body_stmt, expr_stmt(assign(ident("value", int_type()),
                                        binary("-", ident("value", int_type()),
                                               ident("step", int_type())))));
    auto loop = std::make_unique<Stmt>();
    loop->kind = StmtKind::While;
    loop->expr = ident("value", int_type());
    loop->then_branch = std::move(body_stmt);

    StmtPtr body = block();
    add_to(*body, std::move(loop));
    Function f = make_method("drain", "Counter", {}, std::move(body));

    CodeGen g;
    g.gen_class_method(c, f);

    CHECK(g.error.empty());
    CHECK(assembles(g.out));
}

TEST(global_objects_get_their_constructor_called) {
    Program p;
    p.classes.push_back(make_counter());
    StmtPtr ctor_body = block();
    add_to(*ctor_body, expr_stmt(assign(ident("value", int_type()),
                                        ident("start", int_type()))));
    p.classes[0].methods.push_back(make_method("Counter", "Counter",
                                               {{"start", int_type()}},
                                               std::move(ctor_body), true));

    Global g0;
    g0.name = "counter";
    g0.type = class_type("Counter");
    g0.ctor_args.push_back(num(7));
    p.globals.push_back(std::move(g0));

    CodeGen g;
    int addr = g.add_global("counter", 4);

    gen_global_object_init(g, p);
    CHECK(g.error.empty());

    // The object's address is an immediate pair: the assembler has no symbols.
    CHECK(contains(g.out, "ldi  r24, " + std::to_string(addr & 0xFF)));
    CHECK(contains(g.out, "ldi  r25, " + std::to_string((addr >> 8) & 0xFF)));
    CHECK(contains(g.out, "pop  r22"));                 // the argument, 7
    CHECK(contains(g.out, "call Counter__ctor"));

    // Emitting the constructor too makes the whole unit self-contained.
    g.gen_class_method(p.classes[0], p.classes[0].methods[0]);
    CHECK(g.error.empty());
    CHECK(assembles(g.out));
}

TEST(a_local_object_is_constructed_in_the_frame) {
    Program p;
    p.classes.push_back(make_counter());
    p.classes[0].methods.push_back(make_method("Counter", "Counter",
                                               {{"start", int_type()}}, nullptr, true));

    // void spawn() { Counter tmp(3); }
    auto decl = std::make_unique<Stmt>();
    decl->kind = StmtKind::VarDecl;
    decl->var_name = "tmp";
    decl->var_type = class_type("Counter");
    decl->ctor_args.push_back(num(3));

    StmtPtr body = block();
    add_to(*body, std::move(decl));
    Function f = make_method("spawn", "Counter", {}, std::move(body));

    CodeGen g;
    gen_global_object_init(g, p);                 // registers the class
    g.gen_class_method(p.classes[0], f);

    CHECK(g.error.empty());
    CHECK(contains(g.out, "movw r30, r28"));      // the frame slot's address
    CHECK(contains(g.out, "movw r24, r30"));
    CHECK(contains(g.out, "call Counter__ctor"));
    CHECK(assembles(g.out + "\nCounter__ctor:\n    ret\n"));
}

TEST(a_global_object_without_constructor_arguments_emits_nothing) {
    Program p;
    p.classes.push_back(make_counter());

    Global g0;
    g0.name = "counter";
    g0.type = class_type("Counter");
    p.globals.push_back(std::move(g0));

    CodeGen g;
    gen_global_object_init(g, p);

    CHECK(g.error.empty());
    CHECK(g.out.empty());
}

TEST(constructor_arguments_without_a_constructor_are_an_error) {
    Program p;
    p.classes.push_back(make_counter());          // no constructor declared

    Global g0;
    g0.name = "counter";
    g0.type = class_type("Counter");
    g0.ctor_args.push_back(num(1));
    p.globals.push_back(std::move(g0));

    CodeGen g;
    gen_global_object_init(g, p);

    CHECK(!g.error.empty());
}
