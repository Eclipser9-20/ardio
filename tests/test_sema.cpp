#include "harness.h"

#include "ardio/avr/ast.h"
#include "ardio/avr/sema.h"

#include <string>
#include <utility>

using namespace ardio;

namespace {

// --- tiny AST builders, so the tests never depend on the parser -------------

ExprPtr lit(long v, size_t line = 1) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::IntLiteral;
    e->int_value = v;
    e->line = line;
    return e;
}

ExprPtr str(const std::string& s, size_t line = 1) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::StringLiteral;
    e->str_value = s;
    e->line = line;
    return e;
}

ExprPtr ident(const std::string& n, size_t line = 1) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Identifier;
    e->name = n;
    e->line = line;
    return e;
}

ExprPtr binary(const std::string& op, ExprPtr l, ExprPtr r, size_t line = 1) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Binary;
    e->op = op;
    e->lhs = std::move(l);
    e->rhs = std::move(r);
    e->line = line;
    return e;
}

ExprPtr unary(const std::string& op, ExprPtr operand, size_t line = 1) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Unary;
    e->op = op;
    e->lhs = std::move(operand);
    e->line = line;
    return e;
}

ExprPtr assign(ExprPtr l, ExprPtr r, const std::string& op = "=", size_t line = 1) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Assign;
    e->op = op;
    e->lhs = std::move(l);
    e->rhs = std::move(r);
    e->line = line;
    return e;
}

ExprPtr index(ExprPtr base, ExprPtr sub, size_t line = 1) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Index;
    e->lhs = std::move(base);
    e->rhs = std::move(sub);
    e->line = line;
    return e;
}

ExprPtr member(ExprPtr obj, const std::string& name, bool arrow = false, size_t line = 1) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Member;
    e->lhs = std::move(obj);
    e->name = name;
    e->through_pointer = arrow;
    e->line = line;
    return e;
}

ExprPtr call(const std::string& name, std::vector<ExprPtr> args, size_t line = 1) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Call;
    e->name = name;
    e->args = std::move(args);
    e->line = line;
    return e;
}

StmtPtr expr_stmt(ExprPtr e, size_t line = 1) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Expression;
    s->line = line;
    s->expr = std::move(e);
    return s;
}

StmtPtr var(const std::string& name, TypePtr type, ExprPtr init = nullptr, size_t line = 1) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::VarDecl;
    s->var_name = name;
    s->var_type = std::move(type);
    s->var_init = std::move(init);
    s->line = line;
    return s;
}

StmtPtr ret(ExprPtr e, size_t line = 1) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Return;
    s->expr = std::move(e);
    s->line = line;
    return s;
}

StmtPtr block(std::vector<StmtPtr> body) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Block;
    s->body = std::move(body);
    return s;
}

// Builds `void <name>() { <body> }` and appends it to the program.
Function& add_fn(Program& p, const std::string& name, std::vector<StmtPtr> body) {
    Function f;
    f.name = name;
    f.return_type = make_type(TypeKind::Void);
    f.body = block(std::move(body));
    p.functions.push_back(std::move(f));
    return p.functions.back();
}

// Reaches the single expression statement at index i of the first function.
const Expr& stmt_expr(const Program& p, size_t i, size_t fn = 0) {
    return *p.functions[fn].body->body[i]->expr;
}

} // namespace

// --------------------------------------------------------- literal types ---

TEST(sema_int_literal_is_int_and_big_literal_is_long) {
    Program p;
    add_fn(p, "setup", [] {
        std::vector<StmtPtr> b;
        b.push_back(expr_stmt(lit(7)));
        b.push_back(expr_stmt(lit(100000)));
        return b;
    }());
    auto r = analyse(p);
    CHECK(r.ok);
    CHECK(stmt_expr(p, 0).type->kind == TypeKind::Int);
    CHECK(stmt_expr(p, 1).type->kind == TypeKind::Long);
}

TEST(sema_string_literal_is_a_char_array_including_the_terminator) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(str("hi")));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    const Expr& e = stmt_expr(p, 0);
    CHECK(e.type->kind == TypeKind::Array);
    CHECK_EQ(e.type->array_length, 3L);
    CHECK(e.type->pointee->kind == TypeKind::Char);
}

// ------------------------------------------------- arithmetic conversions ---

TEST(sema_char_plus_char_promotes_to_int) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("a", make_type(TypeKind::Char)));
    b.push_back(var("b", make_type(TypeKind::Char)));
    b.push_back(expr_stmt(binary("+", ident("a"), ident("b"))));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    CHECK(stmt_expr(p, 2).type->kind == TypeKind::Int);
}

TEST(sema_int_plus_long_widens_to_long) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("i", make_type(TypeKind::Int)));
    b.push_back(var("l", make_type(TypeKind::Long)));
    b.push_back(expr_stmt(binary("*", ident("i"), ident("l"))));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    CHECK(stmt_expr(p, 2).type->kind == TypeKind::Long);
}

TEST(sema_unsigned_wins_at_equal_rank) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("i", make_type(TypeKind::Int)));
    b.push_back(var("u", make_type(TypeKind::UInt)));
    b.push_back(expr_stmt(binary("-", ident("i"), ident("u"))));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    CHECK(stmt_expr(p, 2).type->kind == TypeKind::UInt);
}

TEST(sema_shift_keeps_the_left_operand_type) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("mask", make_type(TypeKind::Char)));
    b.push_back(var("n", make_type(TypeKind::Long)));
    b.push_back(expr_stmt(binary("<<", ident("mask"), ident("n"))));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    // char promotes to int; the long on the right must not widen the result.
    CHECK(stmt_expr(p, 2).type->kind == TypeKind::Int);
}

TEST(sema_comparisons_and_logic_yield_bool) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("i", make_type(TypeKind::Int)));
    b.push_back(expr_stmt(binary("<", ident("i"), lit(3))));
    b.push_back(expr_stmt(binary("&&", ident("i"), lit(1))));
    b.push_back(expr_stmt(unary("!", ident("i"))));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    CHECK(stmt_expr(p, 1).type->kind == TypeKind::Bool);
    CHECK(stmt_expr(p, 2).type->kind == TypeKind::Bool);
    CHECK(stmt_expr(p, 3).type->kind == TypeKind::Bool);
}

// --------------------------------------------- pointers, arrays, indexing ---

TEST(sema_pointer_plus_int_stays_a_pointer) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("q", make_pointer(make_type(TypeKind::Long))));
    b.push_back(expr_stmt(binary("+", ident("q"), lit(2))));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    const Expr& e = stmt_expr(p, 1);
    CHECK(e.type->kind == TypeKind::Pointer);
    // The back end scales the offset by this size: 2 elements is 8 bytes.
    CHECK_EQ(e.type->pointee->size(), 4);
}

TEST(sema_pointer_difference_is_an_int) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("a", make_pointer(make_type(TypeKind::Char))));
    b.push_back(var("b", make_pointer(make_type(TypeKind::Char))));
    b.push_back(expr_stmt(binary("-", ident("a"), ident("b"))));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    CHECK(stmt_expr(p, 2).type->kind == TypeKind::Int);
}

TEST(sema_array_decays_to_pointer_in_an_expression) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("buf", make_array(make_type(TypeKind::Int), 8)));
    b.push_back(expr_stmt(binary("+", ident("buf"), lit(1))));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    const Expr& e = stmt_expr(p, 1);
    CHECK(e.type->kind == TypeKind::Pointer);
    CHECK(e.type->pointee->kind == TypeKind::Int);
    // The declaration itself keeps its array type, so storage is still 16 bytes.
    CHECK_EQ(p.functions[0].body->body[0]->var_type->size(), 16);
}

TEST(sema_subscript_and_deref_give_the_element_type) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("buf", make_array(make_type(TypeKind::Long), 4)));
    b.push_back(var("q", make_pointer(make_type(TypeKind::Char))));
    b.push_back(expr_stmt(index(ident("buf"), lit(2))));
    b.push_back(expr_stmt(unary("*", ident("q"))));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    CHECK(stmt_expr(p, 2).type->kind == TypeKind::Long);
    CHECK(stmt_expr(p, 3).type->kind == TypeKind::Char);
}

TEST(sema_address_of_produces_a_pointer_to_the_operand) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("x", make_type(TypeKind::Int)));
    b.push_back(expr_stmt(unary("&", ident("x"))));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    const Expr& e = stmt_expr(p, 1);
    CHECK(e.type->kind == TypeKind::Pointer);
    CHECK(e.type->pointee->kind == TypeKind::Int);
}

TEST(sema_rejects_dereferencing_a_non_pointer) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("x", make_type(TypeKind::Int)));
    b.push_back(expr_stmt(unary("*", ident("x", 9), 9), 9));
    add_fn(p, "setup", std::move(b));
    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 9: cannot dereference int");
}

// ------------------------------------------------------------ assignment ---

TEST(sema_assignment_narrowing_and_widening_are_allowed) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("c", make_type(TypeKind::Char)));
    b.push_back(var("l", make_type(TypeKind::Long)));
    b.push_back(expr_stmt(assign(ident("c"), ident("l"))));     // narrowing
    b.push_back(expr_stmt(assign(ident("l"), ident("c"))));     // widening
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    CHECK(stmt_expr(p, 2).type->kind == TypeKind::Char);
    CHECK(stmt_expr(p, 3).type->kind == TypeKind::Long);
}

TEST(sema_rejects_assignment_to_a_non_lvalue) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("x", make_type(TypeKind::Int)));
    b.push_back(expr_stmt(assign(binary("+", ident("x", 4), lit(1, 4), 4), lit(3, 4), "=", 4), 4));
    add_fn(p, "setup", std::move(b));
    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 4: assignment to a non-lvalue");
}

TEST(sema_rejects_assignment_to_a_literal) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(assign(lit(1, 2), lit(3, 2), "=", 2), 2));
    add_fn(p, "setup", std::move(b));
    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 2: assignment to a non-lvalue");
}

TEST(sema_rejects_assigning_a_pointer_to_an_int) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("x", make_type(TypeKind::Int)));
    b.push_back(var("q", make_pointer(make_type(TypeKind::Char))));
    b.push_back(expr_stmt(assign(ident("q", 5), ident("x", 5), "=", 5), 5));
    add_fn(p, "setup", std::move(b));
    // int -> pointer is tolerated (null constants); pointer -> int is not.
    CHECK(analyse(p).ok);

    Program q;
    std::vector<StmtPtr> b2;
    b2.push_back(var("s", make_type(TypeKind::Char)));
    b2.push_back(var("r", make_pointer(make_type(TypeKind::Char))));
    b2.push_back(expr_stmt(assign(ident("s", 6), ident("r", 6), "=", 6), 6));
    add_fn(q, "setup", std::move(b2));
    auto res = analyse(q);
    CHECK(!res.ok);
    CHECK(res.error == "line 6: cannot assign char* to char");
}

TEST(sema_compound_assignment_on_a_pointer_keeps_the_pointer) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(var("q", make_pointer(make_type(TypeKind::Int))));
    b.push_back(expr_stmt(assign(ident("q"), lit(1), "+=")));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    CHECK(stmt_expr(p, 1).type->kind == TypeKind::Pointer);
}

// ------------------------------------------------------ names and scopes ---

TEST(sema_rejects_an_undeclared_identifier) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(ident("ledPin", 12), 12));
    add_fn(p, "setup", std::move(b));
    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 12: undeclared identifier 'ledPin'");
}

TEST(sema_sees_globals_from_inside_a_function) {
    Program p;
    Global g;
    g.name = "ledPin";
    g.type = make_type(TypeKind::Int);
    g.init = lit(13);
    g.line = 1;
    p.globals.push_back(std::move(g));

    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(ident("ledPin")));
    add_fn(p, "setup", std::move(b));
    CHECK(analyse(p).ok);
    CHECK(stmt_expr(p, 0).type->kind == TypeKind::Int);
}

TEST(sema_block_locals_do_not_leak_out) {
    Program p;
    std::vector<StmtPtr> inner;
    inner.push_back(var("tmp", make_type(TypeKind::Int)));

    std::vector<StmtPtr> b;
    b.push_back(block(std::move(inner)));
    b.push_back(expr_stmt(ident("tmp", 20), 20));
    add_fn(p, "setup", std::move(b));
    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 20: undeclared identifier 'tmp'");
}

TEST(sema_reports_only_the_first_error) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(ident("first", 3), 3));
    b.push_back(expr_stmt(ident("second", 4), 4));
    add_fn(p, "setup", std::move(b));
    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 3: undeclared identifier 'first'");
}

// ----------------------------------------------------------------- calls ---

TEST(sema_call_takes_the_return_type_and_checks_arity) {
    Program p;
    Function fn;
    fn.name = "add";
    fn.return_type = make_type(TypeKind::Long);
    fn.params.push_back({"a", make_type(TypeKind::Int)});
    fn.params.push_back({"b", make_type(TypeKind::Int)});
    fn.body = block({});
    p.functions.push_back(std::move(fn));

    std::vector<ExprPtr> args;
    args.push_back(lit(1));
    args.push_back(lit(2));
    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("add", std::move(args))));
    add_fn(p, "setup", std::move(b));

    CHECK(analyse(p).ok);
    CHECK(p.functions[1].body->body[0]->expr->type->kind == TypeKind::Long);
}

TEST(sema_rejects_the_wrong_argument_count) {
    Program p;
    Function fn;
    fn.name = "digitalWrite";
    fn.return_type = make_type(TypeKind::Void);
    fn.params.push_back({"pin", make_type(TypeKind::Int)});
    fn.params.push_back({"value", make_type(TypeKind::Int)});
    p.functions.push_back(std::move(fn));

    std::vector<ExprPtr> args;
    args.push_back(lit(13, 8));
    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("digitalWrite", std::move(args), 8), 8));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 8: 'digitalWrite' expects 2 arguments, got 1");
}

TEST(sema_rejects_an_undeclared_function) {
    Program p;
    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("delay", {}, 30), 30));
    add_fn(p, "setup", std::move(b));
    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 30: undeclared function 'delay'");
}

TEST(sema_checks_return_against_the_function_type) {
    Program p;
    Function fn;
    fn.name = "value";
    fn.return_type = make_type(TypeKind::Int);
    std::vector<StmtPtr> body;
    body.push_back(ret(lit(5)));
    fn.body = block(std::move(body));
    p.functions.push_back(std::move(fn));
    CHECK(analyse(p).ok);

    Program q;
    Function bad;
    bad.name = "nothing";
    bad.return_type = make_type(TypeKind::Void);
    std::vector<StmtPtr> bbody;
    bbody.push_back(ret(lit(5, 11), 11));
    bad.body = block(std::move(bbody));
    q.functions.push_back(std::move(bad));
    auto r = analyse(q);
    CHECK(!r.ok);
    CHECK(r.error == "line 11: return with a value in a function returning void");
}

// ---------------------------------------------------------- class layout ---

TEST(sema_lays_out_class_fields_byte_packed) {
    Program p;
    ClassDecl c;
    c.name = "Servo";
    c.fields.push_back({"pin", make_type(TypeKind::Char), 0});
    c.fields.push_back({"angle", make_type(TypeKind::Int), 0});
    c.fields.push_back({"ticks", make_type(TypeKind::Long), 0});
    c.fields.push_back({"target", make_pointer(make_type(TypeKind::Int)), 0});
    p.classes.push_back(std::move(c));

    CHECK(analyse(p).ok);
    const ClassDecl& s = p.classes[0];
    CHECK_EQ(s.fields[0].offset, 0);   // char at 0
    CHECK_EQ(s.fields[1].offset, 1);   // int at 1 -- no padding on AVR
    CHECK_EQ(s.fields[2].offset, 3);
    CHECK_EQ(s.fields[3].offset, 7);
    CHECK_EQ(s.size, 9);
}

TEST(sema_class_size_is_visible_through_the_type) {
    Program p;
    ClassDecl c;
    c.name = "Pair";
    c.fields.push_back({"a", make_type(TypeKind::Int), 0});
    c.fields.push_back({"b", make_type(TypeKind::Int), 0});
    p.classes.push_back(std::move(c));
    CHECK(analyse(p).ok);

    auto t = make_type(TypeKind::Class);
    t->class_name = "Pair";
    CHECK_EQ(t->size(), 4);
    CHECK_EQ(make_array(t, 3)->size(), 12);
}

TEST(sema_nested_class_field_contributes_its_whole_size) {
    Program p;
    ClassDecl inner;
    inner.name = "Point";
    inner.fields.push_back({"x", make_type(TypeKind::Int), 0});
    inner.fields.push_back({"y", make_type(TypeKind::Int), 0});

    auto point = make_type(TypeKind::Class);
    point->class_name = "Point";

    ClassDecl outer;
    outer.name = "Line";
    outer.fields.push_back({"from", point, 0});
    outer.fields.push_back({"to", point, 0});
    outer.fields.push_back({"width", make_type(TypeKind::Char), 0});

    // Declared out of order on purpose: layout must resolve Point first.
    p.classes.push_back(std::move(outer));
    p.classes.push_back(std::move(inner));

    CHECK(analyse(p).ok);
    CHECK_EQ(p.classes[1].size, 4);
    CHECK_EQ(p.classes[0].fields[1].offset, 4);
    CHECK_EQ(p.classes[0].fields[2].offset, 8);
    CHECK_EQ(p.classes[0].size, 9);
}

TEST(sema_member_access_uses_the_field_type) {
    Program p;
    ClassDecl c;
    c.name = "Led";
    c.fields.push_back({"pin", make_type(TypeKind::Char), 0});
    c.fields.push_back({"onFor", make_type(TypeKind::Long), 0});
    p.classes.push_back(std::move(c));

    auto led = make_type(TypeKind::Class);
    led->class_name = "Led";

    std::vector<StmtPtr> b;
    b.push_back(var("led", led));
    b.push_back(var("q", make_pointer(led)));
    b.push_back(expr_stmt(member(ident("led"), "onFor")));
    b.push_back(expr_stmt(member(ident("q"), "pin", true)));
    add_fn(p, "setup", std::move(b));

    CHECK(analyse(p).ok);
    CHECK(stmt_expr(p, 2).type->kind == TypeKind::Long);
    CHECK(stmt_expr(p, 3).type->kind == TypeKind::Char);
}

TEST(sema_rejects_an_unknown_member) {
    Program p;
    ClassDecl c;
    c.name = "Led";
    c.fields.push_back({"pin", make_type(TypeKind::Char), 0});
    p.classes.push_back(std::move(c));

    auto led = make_type(TypeKind::Class);
    led->class_name = "Led";

    std::vector<StmtPtr> b;
    b.push_back(var("led", led));
    b.push_back(expr_stmt(member(ident("led", 15), "brightness", false, 15), 15));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 15: class 'Led' has no member 'brightness'");
}

TEST(sema_method_body_sees_its_own_fields) {
    Program p;
    ClassDecl c;
    c.name = "Counter";
    c.fields.push_back({"count", make_type(TypeKind::Int), 0});

    Function m;
    m.name = "bump";
    m.owner_class = "Counter";
    m.return_type = make_type(TypeKind::Int);
    std::vector<StmtPtr> body;
    body.push_back(expr_stmt(assign(ident("count"), binary("+", ident("count"), lit(1)))));
    body.push_back(ret(ident("count")));
    m.body = block(std::move(body));
    c.methods.push_back(std::move(m));
    p.classes.push_back(std::move(c));

    auto r = analyse(p);
    CHECK(r.ok);
    CHECK(r.error.empty());
}

// ------------------------------------------------------------ overloading ---

namespace {

// Builds a free function `<ret> <name>(<params>)` with an empty body and
// appends it, so a test can declare a whole overload set in a few lines.
Function& add_overload(Program& p, const std::string& name, std::vector<TypeKind> params,
                       TypeKind ret = TypeKind::Void) {
    Function f;
    f.name = name;
    f.return_type = make_type(ret);
    for (size_t i = 0; i < params.size(); ++i)
        f.params.push_back({"p" + std::to_string(i), make_type(params[i])});
    f.body = block({});
    p.functions.push_back(std::move(f));
    return p.functions.back();
}

// The same, as a method of `c`.
Function& add_method(ClassDecl& c, const std::string& name, std::vector<TypePtr> params,
                     TypeKind ret = TypeKind::Void) {
    Function m;
    m.name = name;
    m.owner_class = c.name;
    m.return_type = make_type(ret);
    for (size_t i = 0; i < params.size(); ++i)
        m.params.push_back({"p" + std::to_string(i), params[i]});
    m.body = block({});
    c.methods.push_back(std::move(m));
    return c.methods.back();
}

// Finds the symbol a definition ended up with, by position.
const std::string& fn_symbol(const Program& p, size_t i) { return p.functions[i].name; }

// A program's defaults are global state; every test that sets one clears it
// first so the tests stay independent of each other's order.
struct Defaults {
    Defaults() { clear_default_arguments(); }
    ~Defaults() { clear_default_arguments(); }
};

} // namespace

TEST(sema_leaves_a_name_with_one_definition_completely_alone) {
    Program p;
    add_overload(p, "delay", {TypeKind::Long});
    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("delay", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(100));
        return a;
    }())));
    add_fn(p, "setup", std::move(b));

    CHECK(analyse(p).ok);
    CHECK(fn_symbol(p, 0) == "delay");
    CHECK(stmt_expr(p, 0, 1).name == "delay");
}

TEST(sema_picks_an_overload_by_argument_count) {
    Program p;
    add_overload(p, "attach", {TypeKind::Int});
    add_overload(p, "attach", {TypeKind::Int, TypeKind::Int, TypeKind::Int});

    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("attach", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(9));
        return a;
    }())));
    b.push_back(expr_stmt(call("attach", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(9));
        a.push_back(lit(544));
        a.push_back(lit(2400));
        return a;
    }())));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(r.ok);
    CHECK(r.error.empty());
    // Both definitions were renamed, and each call points at one of them.
    CHECK(fn_symbol(p, 0) == "attach__int");
    CHECK(fn_symbol(p, 1) == "attach__int_int_int");
    CHECK(stmt_expr(p, 0, 2).name == "attach__int");
    CHECK(stmt_expr(p, 1, 2).name == "attach__int_int_int");
}

TEST(sema_picks_an_overload_by_argument_type) {
    Program p;
    add_overload(p, "write", {TypeKind::Char});
    add_overload(p, "write", {TypeKind::Long});

    std::vector<StmtPtr> b;
    b.push_back(var("c", make_type(TypeKind::Char)));
    b.push_back(var("l", make_type(TypeKind::Long)));
    b.push_back(expr_stmt(call("write", [] {
        std::vector<ExprPtr> a;
        a.push_back(ident("c"));
        return a;
    }())));
    b.push_back(expr_stmt(call("write", [] {
        std::vector<ExprPtr> a;
        a.push_back(ident("l"));
        return a;
    }())));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(r.ok);
    CHECK(stmt_expr(p, 2, 2).name == "write__char");
    CHECK(stmt_expr(p, 3, 2).name == "write__long");
}

TEST(sema_prefers_an_exact_match_over_a_converting_one) {
    Program p;
    // Both are assignable from a char, so only the exactness rule separates
    // them -- and only the pointer overload is exact for a string literal.
    add_overload(p, "print", {TypeKind::Int});
    Function f;
    f.name = "print";
    f.return_type = make_type(TypeKind::Void);
    f.params.push_back({"s", make_pointer(make_type(TypeKind::Char))});
    f.body = block({});
    p.functions.push_back(std::move(f));

    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("print", [] {
        std::vector<ExprPtr> a;
        a.push_back(str("hi"));
        return a;
    }())));
    b.push_back(expr_stmt(call("print", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(7));
        return a;
    }())));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(r.ok);
    CHECK(stmt_expr(p, 0, 2).name == "print__pchar");
    CHECK(stmt_expr(p, 1, 2).name == "print__int");
}

TEST(sema_overloads_a_method_and_keeps_the_class_off_the_call_name) {
    Program p;
    ClassDecl c;
    c.name = "TwoWire";
    add_method(c, "write", {make_type(TypeKind::Int)});
    add_method(c, "write", {make_pointer(make_type(TypeKind::Char)), make_type(TypeKind::Int)});
    p.classes.push_back(std::move(c));

    auto wire = make_type(TypeKind::Class);
    wire->class_name = "TwoWire";

    std::vector<StmtPtr> b;
    b.push_back(var("w", wire));
    auto one = call("write", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(65));
        return a;
    }());
    one->lhs = ident("w");
    b.push_back(expr_stmt(std::move(one)));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(r.ok);
    // The definitions carry the bare member name; the back end prepends the
    // class itself, so `TwoWire__write__int` is the label that results.
    CHECK(p.classes[0].methods[0].name == "write__int");
    CHECK(p.classes[0].methods[1].name == "write__pchar_int");
    CHECK(stmt_expr(p, 1, 0).name == "write__int");
}

TEST(sema_treats_a_prototype_and_its_definition_as_one_function) {
    Program p;
    Function proto;
    proto.name = "beep";
    proto.return_type = make_type(TypeKind::Void);
    proto.params.push_back({"n", make_type(TypeKind::Int)});
    p.functions.push_back(std::move(proto));      // no body
    add_overload(p, "beep", {TypeKind::Int});     // the definition

    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("beep", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(3));
        return a;
    }())));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(r.ok);
    // One signature, so nothing is renamed.
    CHECK(fn_symbol(p, 0) == "beep");
    CHECK(fn_symbol(p, 1) == "beep");
    CHECK(stmt_expr(p, 0, 2).name == "beep");
}

TEST(sema_reports_an_ambiguous_call_instead_of_guessing) {
    Program p;
    add_overload(p, "send", {TypeKind::Int, TypeKind::Long});
    add_overload(p, "send", {TypeKind::Long, TypeKind::Int});

    std::vector<StmtPtr> b;
    b.push_back(var("c", make_type(TypeKind::Char)));
    b.push_back(expr_stmt(call("send", [] {
        std::vector<ExprPtr> a;
        a.push_back(ident("c", 12));
        a.push_back(ident("c", 12));
        return a;
    }(), 12), 12));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 12: call to 'send' is ambiguous; candidates are "
                     "send(int, long), send(long, int)");
}

TEST(sema_reports_when_no_overload_accepts_the_arguments) {
    Program p;
    add_overload(p, "tone", {TypeKind::Int});
    add_overload(p, "tone", {TypeKind::Int, TypeKind::Int});

    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("tone", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(1, 4));
        a.push_back(lit(2, 4));
        a.push_back(lit(3, 4));
        return a;
    }(), 4), 4));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 4: no overload of 'tone' takes these 3 arguments; "
                     "candidates are tone(int), tone(int, int)");
}

TEST(sema_still_rejects_the_wrong_arity_against_a_single_definition) {
    // The one-candidate diagnostic must not change now that overloading works.
    Program p;
    add_overload(p, "pinMode", {TypeKind::Int, TypeKind::Int});
    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("pinMode", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(13, 6));
        return a;
    }(), 6), 6));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 6: 'pinMode' expects 2 arguments, got 1");
}

// ------------------------------------------------------ default arguments ---

TEST(sema_fills_in_a_trailing_default_argument) {
    Defaults guard;
    Program p;
    add_overload(p, "attach", {TypeKind::Int, TypeKind::Int, TypeKind::Int});
    set_default_argument("", "attach", 3, 1, 544);
    set_default_argument("", "attach", 3, 2, 2400);

    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("attach", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(9));
        return a;
    }())));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(r.ok);
    CHECK(r.error.empty());
    // The call now carries a complete argument list.
    const Expr& c = stmt_expr(p, 0, 1);
    CHECK_EQ(c.args.size(), size_t(3));
    CHECK_EQ(c.args[0]->int_value, 9L);
    CHECK_EQ(c.args[1]->int_value, 544L);
    CHECK_EQ(c.args[2]->int_value, 2400L);
    CHECK(c.args[2]->type->kind == TypeKind::Int);
}

TEST(sema_accepts_a_default_being_supplied_explicitly) {
    Defaults guard;
    Program p;
    add_overload(p, "random", {TypeKind::Long, TypeKind::Long}, TypeKind::Long);
    set_default_argument("", "random", 2, 1, 0);

    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("random", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(10));
        a.push_back(lit(20));
        return a;
    }())));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(r.ok);
    const Expr& c = stmt_expr(p, 0, 1);
    CHECK_EQ(c.args.size(), size_t(2));
    CHECK_EQ(c.args[1]->int_value, 20L);
}

TEST(sema_rejects_a_call_below_the_required_argument_count) {
    Defaults guard;
    Program p;
    add_overload(p, "attach", {TypeKind::Int, TypeKind::Int, TypeKind::Int});
    set_default_argument("", "attach", 3, 2, 2400);   // only the last is optional

    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("attach", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(9, 21));
        return a;
    }(), 21), 21));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(!r.ok);
    CHECK(r.error == "line 21: 'attach' expects between 2 and 3 arguments, got 1");
}

TEST(sema_fills_a_method_default_and_still_resolves_the_overload) {
    Defaults guard;
    Program p;
    ClassDecl c;
    c.name = "LiquidCrystal_I2C";
    add_method(c, "print", {make_pointer(make_type(TypeKind::Char))});
    add_method(c, "print", {make_type(TypeKind::Long), make_type(TypeKind::Int)});
    p.classes.push_back(std::move(c));
    // print(long value, int base = 10)
    set_default_argument("LiquidCrystal_I2C", "print", 2, 1, 10);

    auto lcd = make_type(TypeKind::Class);
    lcd->class_name = "LiquidCrystal_I2C";

    std::vector<StmtPtr> b;
    b.push_back(var("lcd", lcd));
    auto number = call("print", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(42));
        return a;
    }());
    number->lhs = ident("lcd");
    b.push_back(expr_stmt(std::move(number)));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(r.ok);
    const Expr& called = stmt_expr(p, 1, 0);
    CHECK(called.name == "print__long_int");
    CHECK_EQ(called.args.size(), size_t(2));
    if (called.args.size() == 2) CHECK_EQ(called.args[1]->int_value, 10L);
}

TEST(sema_lets_a_default_settle_an_otherwise_equal_arity_choice) {
    Defaults guard;
    Program p;
    add_overload(p, "beep", {TypeKind::Int});
    add_overload(p, "beep", {TypeKind::Int, TypeKind::Int});
    set_default_argument("", "beep", 2, 1, 5);

    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("beep", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(1));
        return a;
    }())));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    // Supplying every argument beats leaning on a default, so the one-parameter
    // overload wins outright rather than the call being called ambiguous.
    CHECK(r.ok);
    CHECK(stmt_expr(p, 0, 2).name == "beep__int");
}

TEST(sema_defaults_do_not_leak_between_same_named_overloads) {
    Defaults guard;
    Program p;
    add_overload(p, "send", {TypeKind::Int});
    add_overload(p, "send", {TypeKind::Int, TypeKind::Int});
    // Only the two-parameter overload has a default.
    set_default_argument("", "send", 2, 1, 3);

    std::vector<StmtPtr> b;
    b.push_back(expr_stmt(call("send", [] {
        std::vector<ExprPtr> a;
        a.push_back(lit(1));
        a.push_back(lit(2));
        return a;
    }())));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(r.ok);
    CHECK(stmt_expr(p, 0, 2).name == "send__int_int");
    CHECK_EQ(stmt_expr(p, 0, 2).args.size(), size_t(2));
}

TEST(sema_fills_a_constructor_default) {
    Defaults guard;
    Program p;
    ClassDecl c;
    c.name = "Stepper";
    Function ctor;
    ctor.name = "Stepper";
    ctor.is_constructor = true;
    ctor.return_type = make_type(TypeKind::Void);
    ctor.params.push_back({"steps", make_type(TypeKind::Int)});
    ctor.params.push_back({"speed", make_type(TypeKind::Int)});
    ctor.body = block({});
    c.methods.push_back(std::move(ctor));
    p.classes.push_back(std::move(c));
    set_default_argument("Stepper", "Stepper", 2, 1, 60);

    auto stepper = make_type(TypeKind::Class);
    stepper->class_name = "Stepper";

    auto decl = var("motor", stepper);
    decl->ctor_args.push_back(lit(200));
    std::vector<StmtPtr> b;
    b.push_back(std::move(decl));
    add_fn(p, "setup", std::move(b));

    auto r = analyse(p);
    CHECK(r.ok);
    // Constructors keep their `<Class>__ctor` label, but their arguments are
    // completed like anything else's.
    CHECK(p.classes[0].methods[0].name == "Stepper");
    CHECK_EQ(p.functions[0].body->body[0]->ctor_args.size(), size_t(2));
    CHECK_EQ(p.functions[0].body->body[0]->ctor_args[1]->int_value, 60L);
}
