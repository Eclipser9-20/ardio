#include "harness.h"

#include <string>
#include <vector>

#include "ardio/avr/parser.h"
#include "ardio/avr/token.h"

using namespace ardio;

namespace {

ParseResult parse_text(const std::string& source) { return parse(tokenize(source)); }

Program parse_ok(const std::string& source) {
    ParseResult result = parse_text(source);
    CHECK(result.ok);
    CHECK(result.error.empty());
    return std::move(result.program);
}

const Function* find_function(const Program& program, const std::string& name) {
    for (const auto& fn : program.functions)
        if (fn.name == name) return &fn;
    return nullptr;
}

const Global* find_global(const Program& program, const std::string& name) {
    for (const auto& g : program.globals)
        if (g.name == name) return &g;
    return nullptr;
}

// Renders an expression as a fully parenthesised string so precedence and
// associativity can be asserted with a single comparison.
std::string render(const Expr* expr) {
    if (!expr) return "<null>";
    switch (expr->kind) {
        case ExprKind::IntLiteral:
            return std::to_string(expr->int_value);
        case ExprKind::StringLiteral:
            return "\"" + expr->str_value + "\"";
        case ExprKind::InitList: {
            std::string out = "{";
            for (size_t i = 0; i < expr->args.size(); ++i) {
                if (i) out += ", ";
                out += render(expr->args[i].get());
            }
            return out + "}";
        }
        case ExprKind::Identifier:
            return expr->name;
        case ExprKind::Unary:
            return expr->is_postfix ? "(" + render(expr->lhs.get()) + expr->op + ")"
                                    : "(" + expr->op + render(expr->lhs.get()) + ")";
        case ExprKind::Binary:
        case ExprKind::Assign:
            return "(" + render(expr->lhs.get()) + " " + expr->op + " " +
                   render(expr->rhs.get()) + ")";
        case ExprKind::Call: {
            std::string out = "call " + expr->name + "(";
            for (size_t i = 0; i < expr->args.size(); ++i) {
                if (i) out += ", ";
                out += render(expr->args[i].get());
            }
            return out + ")";
        }
        case ExprKind::Index:
            return render(expr->lhs.get()) + "[" + render(expr->rhs.get()) + "]";
        case ExprKind::Member:
            return "(" + render(expr->lhs.get()) + (expr->through_pointer ? "->" : ".") +
                   expr->name + ")";
        case ExprKind::Cast:
            return "(cast " + render(expr->lhs.get()) + ")";
        case ExprKind::Conditional:
            return "(" + render(expr->lhs.get()) + " ? " + render(expr->rhs.get()) + " : " +
                   render(expr->third.get()) + ")";
    }
    return "<unknown>";
}

// Parses a single expression by wrapping it in a return statement.
std::string expr_of(const std::string& text) {
    ParseResult result = parse_text("int probe() { return " + text + "; }");
    if (!result.ok) return "error: " + result.error;
    const Function* fn = find_function(result.program, "probe");
    if (!fn || !fn->body || fn->body->body.empty()) return "<no body>";
    return render(fn->body->body[0]->expr.get());
}

} // namespace

TEST(parser_empty_input_is_an_empty_program) {
    Program program = parse_ok("");
    CHECK_EQ(program.functions.size(), size_t(0));
    CHECK_EQ(program.globals.size(), size_t(0));
    CHECK_EQ(program.classes.size(), size_t(0));
}

TEST(parser_global_with_initialiser) {
    Program program = parse_ok("int counter = 7; unsigned char mask = 0;");
    CHECK_EQ(program.globals.size(), size_t(2));

    const Global* counter = find_global(program, "counter");
    CHECK(counter != nullptr);
    CHECK(counter->type->kind == TypeKind::Int);
    CHECK(counter->init != nullptr);
    CHECK_EQ(counter->init->int_value, 7L);

    const Global* mask = find_global(program, "mask");
    CHECK(mask != nullptr);
    CHECK(mask->type->kind == TypeKind::Char);
    CHECK(!mask->type->is_signed);
}

TEST(parser_global_pointer_and_array_types) {
    Program program = parse_ok("char* name; int table[4]; long total;");
    const Global* name = find_global(program, "name");
    CHECK(name != nullptr);
    CHECK(name->type->kind == TypeKind::Pointer);
    CHECK(name->type->pointee->kind == TypeKind::Char);

    const Global* table = find_global(program, "table");
    CHECK(table != nullptr);
    CHECK(table->type->kind == TypeKind::Array);
    CHECK_EQ(table->type->array_length, 4L);
    CHECK(table->type->pointee->kind == TypeKind::Int);

    const Global* total = find_global(program, "total");
    CHECK(total != nullptr);
    CHECK(total->type->kind == TypeKind::Long);
}

TEST(parser_global_with_constructor_arguments) {
    Program program = parse_ok("class Timer { public: int ticks; };\nTimer clock(3, 4);");
    const Global* clock = find_global(program, "clock");
    CHECK(clock != nullptr);
    CHECK(clock->type->kind == TypeKind::Class);
    CHECK(clock->type->class_name == std::string("Timer"));
    CHECK_EQ(clock->ctor_args.size(), size_t(2));
    CHECK_EQ(clock->ctor_args[0]->int_value, 3L);
    CHECK_EQ(clock->ctor_args[1]->int_value, 4L);
    CHECK(clock->init == nullptr);
}

TEST(parser_multiple_declarators_share_a_base_type) {
    Program program = parse_ok("int a = 1, b, *c;");
    CHECK_EQ(program.globals.size(), size_t(3));
    CHECK(program.globals[0].name == std::string("a"));
    CHECK(program.globals[1].name == std::string("b"));
    CHECK(program.globals[2].name == std::string("c"));
    CHECK(program.globals[2].type->kind == TypeKind::Pointer);
}

TEST(parser_function_declaration_has_no_body) {
    Program program = parse_ok("void setup(void);");
    const Function* setup = find_function(program, "setup");
    CHECK(setup != nullptr);
    CHECK(setup->body == nullptr);
    CHECK(setup->return_type->kind == TypeKind::Void);
    CHECK_EQ(setup->params.size(), size_t(0));
}

TEST(parser_function_definition_with_parameters) {
    Program program = parse_ok("int add(int a, char* b) { return a; }");
    const Function* add = find_function(program, "add");
    CHECK(add != nullptr);
    CHECK(add->body != nullptr);
    CHECK(add->body->kind == StmtKind::Block);
    CHECK_EQ(add->params.size(), size_t(2));
    CHECK(add->params[0].name == std::string("a"));
    CHECK(add->params[0].type->kind == TypeKind::Int);
    CHECK(add->params[1].name == std::string("b"));
    CHECK(add->params[1].type->kind == TypeKind::Pointer);
    CHECK(add->owner_class.empty());
}

TEST(parser_class_fields_methods_and_constructor) {
    Program program = parse_ok(
        "class Led {\n"
        "public:\n"
        "    Led(int pin) { pin_ = pin; }\n"
        "    void on() { write(1); }\n"
        "    int read();\n"
        "private:\n"
        "    int pin_;\n"
        "    char state;\n"
        "};\n");
    CHECK_EQ(program.classes.size(), size_t(1));
    const ClassDecl& led = program.classes[0];
    CHECK(led.name == std::string("Led"));
    CHECK_EQ(led.fields.size(), size_t(2));
    CHECK(led.fields[0].name == std::string("pin_"));
    CHECK(led.fields[1].name == std::string("state"));
    CHECK_EQ(led.methods.size(), size_t(3));

    CHECK(led.methods[0].is_constructor);
    CHECK(led.methods[0].name == std::string("Led"));
    CHECK(led.methods[0].owner_class == std::string("Led"));
    CHECK_EQ(led.methods[0].params.size(), size_t(1));

    CHECK(!led.methods[1].is_constructor);
    CHECK(led.methods[1].name == std::string("on"));
    CHECK(led.methods[1].body != nullptr);

    CHECK(led.methods[2].name == std::string("read"));
    CHECK(led.methods[2].body == nullptr);
}

TEST(parser_struct_parses_like_a_class) {
    Program program = parse_ok("struct Point { int x; int y; };");
    CHECK_EQ(program.classes.size(), size_t(1));
    CHECK(program.classes[0].name == std::string("Point"));
    CHECK_EQ(program.classes[0].fields.size(), size_t(2));
}

TEST(parser_out_of_line_method_definition_joins_its_class) {
    Program program = parse_ok(
        "class Led { public: int read(); };\n"
        "int Led::read() { return 1; }\n");
    CHECK_EQ(program.classes.size(), size_t(1));
    CHECK_EQ(program.classes[0].methods.size(), size_t(2));
    CHECK(program.classes[0].methods[1].name == std::string("read"));
    CHECK(program.classes[0].methods[1].owner_class == std::string("Led"));
    CHECK(program.classes[0].methods[1].body != nullptr);
    CHECK_EQ(program.functions.size(), size_t(0));
}

TEST(parser_enumerators_become_integer_globals) {
    Program program = parse_ok("enum Mode { Idle, Run = 5, Halt };");
    CHECK_EQ(program.globals.size(), size_t(3));
    CHECK(program.globals[0].name == std::string("Idle"));
    CHECK_EQ(program.globals[0].init->int_value, 0L);
    CHECK(program.globals[0].type->kind == TypeKind::Int);
    CHECK(program.globals[1].name == std::string("Run"));
    CHECK_EQ(program.globals[1].init->int_value, 5L);
    CHECK(program.globals[2].name == std::string("Halt"));
    CHECK_EQ(program.globals[2].init->int_value, 6L);
}

TEST(parser_statements_cover_the_control_flow_forms) {
    Program program = parse_ok(
        "void run() {\n"
        "    int i = 0;\n"
        "    if (i) { i = 1; } else i = 2;\n"
        "    while (i) { break; }\n"
        "    for (int j = 0; j < 3; j = j + 1) continue;\n"
        "    ;\n"
        "    return;\n"
        "}\n");
    const Function* run = find_function(program, "run");
    CHECK(run != nullptr);
    const auto& body = run->body->body;
    CHECK_EQ(body.size(), size_t(6));
    CHECK(body[0]->kind == StmtKind::VarDecl);
    CHECK(body[0]->var_name == std::string("i"));
    CHECK(body[0]->var_init != nullptr);

    CHECK(body[1]->kind == StmtKind::If);
    CHECK(body[1]->then_branch->kind == StmtKind::Block);
    CHECK(body[1]->else_branch != nullptr);

    CHECK(body[2]->kind == StmtKind::While);
    CHECK(body[2]->then_branch->body[0]->kind == StmtKind::Break);

    CHECK(body[3]->kind == StmtKind::For);
    CHECK(body[3]->init != nullptr);
    CHECK(body[3]->expr != nullptr);
    CHECK(body[3]->step != nullptr);
    CHECK(body[3]->then_branch->kind == StmtKind::Continue);

    CHECK(body[4]->kind == StmtKind::Empty);
    CHECK(body[5]->kind == StmtKind::Return);
    CHECK(body[5]->expr == nullptr);
}

TEST(parser_do_while_lowers_to_a_block_with_a_while) {
    Program program = parse_ok("void run() { do { x = x + 1; } while (x); }");
    const Function* run = find_function(program, "run");
    CHECK(run != nullptr);
    const Stmt* lowered = run->body->body[0].get();
    CHECK(lowered->kind == StmtKind::Block);
    CHECK_EQ(lowered->body.size(), size_t(2));
    // The body runs once unconditionally, then again under the loop test.
    CHECK(lowered->body[0]->kind == StmtKind::Block);
    CHECK(lowered->body[1]->kind == StmtKind::While);
    CHECK(lowered->body[1]->then_branch != nullptr);
    CHECK(lowered->body[1]->then_branch->kind == StmtKind::Block);
    CHECK(render(lowered->body[1]->expr.get()) == std::string("x"));
}

TEST(parser_local_declaration_with_constructor_arguments) {
    Program program = parse_ok("class Led { public: int p; };\nvoid run() { Led d(9); }");
    const Function* run = find_function(program, "run");
    CHECK(run != nullptr);
    const Stmt* decl = run->body->body[0].get();
    CHECK(decl->kind == StmtKind::VarDecl);
    CHECK(decl->var_type->kind == TypeKind::Class);
    CHECK_EQ(decl->ctor_args.size(), size_t(1));
    CHECK_EQ(decl->ctor_args[0]->int_value, 9L);
}

TEST(parser_binary_precedence_and_associativity) {
    CHECK(expr_of("1 + 2 * 3") == std::string("(1 + (2 * 3))"));
    CHECK(expr_of("1 * 2 + 3") == std::string("((1 * 2) + 3)"));
    CHECK(expr_of("1 - 2 - 3") == std::string("((1 - 2) - 3)"));
    CHECK(expr_of("1 + 2 << 3") == std::string("((1 + 2) << 3)"));
    CHECK(expr_of("a < b == c") == std::string("((a < b) == c)"));
    CHECK(expr_of("a & b ^ c | d") == std::string("(((a & b) ^ c) | d)"));
    CHECK(expr_of("a == b && c") == std::string("((a == b) && c)"));
    CHECK(expr_of("a && b || c") == std::string("((a && b) || c)"));
    CHECK(expr_of("(1 + 2) * 3") == std::string("((1 + 2) * 3)"));
}

TEST(parser_assignment_is_right_associative) {
    CHECK(expr_of("a = b = c") == std::string("(a = (b = c))"));
    CHECK(expr_of("a += b * 2") == std::string("(a += (b * 2))"));
    CHECK(expr_of("a <<= 1") == std::string("(a <<= 1)"));
    CHECK(expr_of("a = b || c") == std::string("(a = (b || c))"));
}

TEST(parser_conditional_expression) {
    CHECK(expr_of("a ? b : c") == std::string("(a ? b : c)"));
    CHECK(expr_of("a ? b : c ? d : e") == std::string("(a ? b : (c ? d : e))"));
    CHECK(expr_of("a + 1 ? b : c") == std::string("((a + 1) ? b : c)"));
}

TEST(parser_unary_and_postfix_operators) {
    CHECK(expr_of("-a") == std::string("(-a)"));
    CHECK(expr_of("!a") == std::string("(!a)"));
    CHECK(expr_of("~a") == std::string("(~a)"));
    CHECK(expr_of("*p") == std::string("(*p)"));
    CHECK(expr_of("&x") == std::string("(&x)"));
    CHECK(expr_of("++i") == std::string("(++i)"));
    CHECK(expr_of("i++") == std::string("(i++)"));
    CHECK(expr_of("-a * b") == std::string("((-a) * b)"));
}

TEST(parser_postfix_call_index_and_member_access) {
    CHECK(expr_of("f(1, 2)") == std::string("call f(1, 2)"));
    CHECK(expr_of("f()") == std::string("call f()"));
    CHECK(expr_of("table[i + 1]") == std::string("table[(i + 1)]"));
    CHECK(expr_of("led.pin") == std::string("(led.pin)"));
    CHECK(expr_of("led->pin") == std::string("(led->pin)"));
    CHECK(expr_of("led.on(3)") == std::string("call on(3)"));
    CHECK(expr_of("a.b[2].c") == std::string("((a.b)[2].c)"));
}

TEST(parser_member_access_records_pointer_form) {
    ParseResult result = parse_text("int probe() { return led->pin; }");
    CHECK(result.ok);
    const Function* fn = find_function(result.program, "probe");
    CHECK(fn != nullptr);
    const Expr* member = fn->body->body[0]->expr.get();
    CHECK(member->kind == ExprKind::Member);
    CHECK(member->through_pointer);
    CHECK(member->name == std::string("pin"));
}

TEST(parser_literals) {
    CHECK(expr_of("42") == std::string("42"));
    CHECK(expr_of("\"hi\"") == std::string("\"hi\""));
    CHECK(expr_of("true") == std::string("1"));
    CHECK(expr_of("false") == std::string("0"));
}

TEST(parser_error_reports_the_line_and_expectation) {
    ParseResult result = parse_text("int a = 1\nint b = 2;\n");
    CHECK(!result.ok);
    CHECK(result.error.rfind("line 2:", 0) == 0);
    CHECK(result.error.find("expected") != std::string::npos);
}

TEST(parser_error_stops_at_the_first_problem) {
    ParseResult result = parse_text("void f() {\n    return 1 +;\n}\nvoid g() { }\n");
    CHECK(!result.ok);
    CHECK(result.error.rfind("line 2:", 0) == 0);
    CHECK(result.program.functions.empty());
}

TEST(parser_error_on_unterminated_block) {
    ParseResult result = parse_text("void f() {\n    int x = 1;\n");
    CHECK(!result.ok);
    CHECK(result.error.find("expected '}'") != std::string::npos);
}

TEST(parser_full_sketch_round_trip) {
    Program program = parse_ok(
        "enum Pin { Builtin = 13 };\n"
        "class Blinker {\n"
        "public:\n"
        "    Blinker(int pin) : pin_(pin) { count_ = 0; }\n"
        "    void tick(unsigned int now);\n"
        "private:\n"
        "    int pin_;\n"
        "    long count_;\n"
        "};\n"
        "void Blinker::tick(unsigned int now) {\n"
        "    count_ += now > 0 ? 1 : 0;\n"
        "    for (int i = 0; i < 8; ++i) {\n"
        "        if (i & 1) continue;\n"
        "        digitalWrite(pin_, i % 2);\n"
        "    }\n"
        "}\n"
        "Blinker blinker(Builtin);\n"
        "int main() {\n"
        "    while (1) blinker.tick(0);\n"
        "    return 0;\n"
        "}\n");
    CHECK_EQ(program.classes.size(), size_t(1));
    CHECK_EQ(program.classes[0].fields.size(), size_t(2));
    CHECK_EQ(program.classes[0].methods.size(), size_t(3));
    CHECK(find_global(program, "Builtin") != nullptr);
    CHECK(find_global(program, "blinker") != nullptr);
    CHECK(find_function(program, "main") != nullptr);
}

// ------------------------------------------------------------------ switch ---
//
// A switch is lowered into nodes the AST already has: a block that binds the
// controlling expression to a compiler-generated local, followed by an
// if/else-if chain over that local.

namespace {

// Returns the body of the only function in `source`, which must parse cleanly.
const Stmt* body_of(const Program& program, const std::string& name) {
    const Function* fn = find_function(program, name);
    if (!fn) return nullptr;
    return fn->body.get();
}

// The lowered form of a switch: statement `index` of f's body must be the block
// holding the temporary declaration and the chain.
const Stmt* lowered_switch(const Program& program, size_t index = 0) {
    const Stmt* fn_body = body_of(program, "f");
    if (!fn_body || fn_body->body.size() <= index) return nullptr;
    return fn_body->body[index].get();
}

std::string first_statement_text(const Stmt* block) {
    if (!block || block->body.empty()) return "<empty>";
    const Stmt* first = block->body[0].get();
    if (!first) return "<null>";
    if (first->kind == StmtKind::Expression) return render(first->expr.get());
    if (first->kind == StmtKind::Return) return "return " + render(first->expr.get());
    if (first->kind == StmtKind::Continue) return "continue";
    if (first->kind == StmtKind::Break) return "break";
    return "<other>";
}

} // namespace

TEST(parser_switch_lowers_to_an_if_chain) {
    Program program = parse_ok(
        "void f() {\n"
        "    switch (state) {\n"
        "        case 1: a(); break;\n"
        "        case 2: b(); break;\n"
        "    }\n"
        "}\n");
    const Stmt* outer = lowered_switch(program);
    CHECK(outer != nullptr);
    CHECK(outer->kind == StmtKind::Block);
    // A plain variable needs no temporary: it is simply named again.
    CHECK_EQ(outer->body.size(), size_t(1));

    const Stmt* chain = outer->body[0].get();
    CHECK(chain->kind == StmtKind::If);
    CHECK(render(chain->expr.get()) == std::string("(state == 1)"));
    CHECK(first_statement_text(chain->then_branch.get()) == std::string("call a()"));

    const Stmt* second = chain->else_branch.get();
    CHECK(second != nullptr);
    CHECK(second->kind == StmtKind::If);
    CHECK(render(second->expr.get()) == std::string("(state == 2)"));
    CHECK(first_statement_text(second->then_branch.get()) == std::string("call b()"));
    CHECK(second->else_branch == nullptr);
}

TEST(parser_switch_on_a_member_repeats_the_member_access) {
    Program program = parse_ok(
        "void f() {\n"
        "    switch (machine->state) {\n"
        "        case 1: a(); break;\n"
        "    }\n"
        "}\n");
    const Stmt* outer = lowered_switch(program);
    CHECK_EQ(outer->body.size(), size_t(1));
    CHECK(render(outer->body[0]->expr.get()) == std::string("((machine->state) == 1)"));
}

TEST(parser_switch_evaluates_the_controlling_expression_once) {
    Program program = parse_ok(
        "void f() {\n"
        "    switch (read()) {\n"
        "        case 1: a(); break;\n"
        "        case 2: b(); break;\n"
        "    }\n"
        "}\n");
    const Stmt* outer = lowered_switch(program);
    CHECK(outer != nullptr);
    CHECK_EQ(outer->body.size(), size_t(2));
    const Stmt* decl = outer->body[0].get();
    CHECK(decl->kind == StmtKind::VarDecl);
    CHECK(decl->var_type->kind == TypeKind::Int);
    CHECK(render(decl->var_init.get()) == std::string("call read()"));
    // Every test compares the temporary, never the call again.
    const Stmt* chain = outer->body[1].get();
    CHECK(render(chain->expr.get()) == "(" + decl->var_name + " == 1)");
    CHECK(render(chain->else_branch->expr.get()) == "(" + decl->var_name + " == 2)");
}

TEST(parser_switch_shares_one_branch_between_adjacent_labels) {
    Program program = parse_ok(
        "void f() {\n"
        "    switch (state) {\n"
        "        case 1:\n"
        "        case 2:\n"
        "        case 3: a(); break;\n"
        "    }\n"
        "}\n");
    const Stmt* outer = lowered_switch(program);
    CHECK(outer != nullptr);
    const Stmt* chain = outer->body[0].get();
    CHECK(chain->kind == StmtKind::If);
    CHECK(render(chain->expr.get()) ==
          std::string("(((state == 1) || (state == 2)) || (state == 3))"));
    CHECK(first_statement_text(chain->then_branch.get()) == std::string("call a()"));
    CHECK(chain->else_branch == nullptr);
}

TEST(parser_switch_default_becomes_the_final_else) {
    Program program = parse_ok(
        "void f() {\n"
        "    switch (state) {\n"
        "        case 1: a(); break;\n"
        "        default: d(); break;\n"
        "    }\n"
        "}\n");
    const Stmt* chain = lowered_switch(program)->body[0].get();
    CHECK(chain->kind == StmtKind::If);
    const Stmt* fallback = chain->else_branch.get();
    CHECK(fallback != nullptr);
    CHECK(fallback->kind == StmtKind::Block);
    CHECK(first_statement_text(fallback) == std::string("call d()"));
}

TEST(parser_switch_default_written_first_still_runs_last) {
    Program program = parse_ok(
        "void f() {\n"
        "    switch (state) {\n"
        "        default: d(); break;\n"
        "        case 1: a(); break;\n"
        "    }\n"
        "}\n");
    const Stmt* outer = lowered_switch(program);
    const Stmt* chain = outer->body[0].get();
    CHECK(chain->kind == StmtKind::If);
    CHECK(render(chain->expr.get()) == std::string("(state == 1)"));
    CHECK(first_statement_text(chain->then_branch.get()) == std::string("call a()"));
    CHECK(chain->else_branch->kind == StmtKind::Block);
    CHECK(first_statement_text(chain->else_branch.get()) == std::string("call d()"));
}

TEST(parser_switch_accepts_a_case_ended_by_return) {
    Program program = parse_ok(
        "int f() {\n"
        "    switch (state) {\n"
        "        case 1: return 7;\n"
        "        case 2: return 8;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    const Stmt* chain = lowered_switch(program)->body[0].get();
    CHECK(first_statement_text(chain->then_branch.get()) == std::string("return 7"));
    CHECK(first_statement_text(chain->else_branch->then_branch.get()) ==
          std::string("return 8"));
}

TEST(parser_switch_accepts_a_braced_case_body_ending_in_break) {
    Program program = parse_ok(
        "void f() {\n"
        "    switch (state) {\n"
        "        case 1: { int n = 2; a(n); break; }\n"
        "        case 2: b(); break;\n"
        "    }\n"
        "}\n");
    const Stmt* chain = lowered_switch(program)->body[0].get();
    const Stmt* branch = chain->then_branch.get();
    CHECK(branch->kind == StmtKind::Block);
    CHECK_EQ(branch->body.size(), size_t(1));
    const Stmt* inner = branch->body[0].get();
    CHECK(inner->kind == StmtKind::Block);
    // The trailing break has been consumed: only the declaration and the call
    // remain, and reaching the end of the block leaves the chain.
    CHECK_EQ(inner->body.size(), size_t(2));
    CHECK(inner->body[0]->kind == StmtKind::VarDecl);
    CHECK(inner->body[1]->kind == StmtKind::Expression);
}

TEST(parser_switch_last_case_needs_no_break) {
    Program program = parse_ok(
        "void f() {\n"
        "    switch (state) {\n"
        "        case 1: a(); break;\n"
        "        case 2: b();\n"
        "    }\n"
        "}\n");
    const Stmt* chain = lowered_switch(program)->body[0].get();
    CHECK(first_statement_text(chain->else_branch->then_branch.get()) ==
          std::string("call b()"));
}

TEST(parser_switch_body_may_be_empty) {
    Program program = parse_ok("void f() { switch (read()) { } }");
    const Stmt* outer = lowered_switch(program);
    CHECK(outer->kind == StmtKind::Block);
    // The controlling expression still runs; there is nothing left to test.
    CHECK_EQ(outer->body.size(), size_t(1));
    CHECK(outer->body[0]->kind == StmtKind::VarDecl);
}

TEST(parser_switch_inside_a_loop_keeps_break_bound_to_its_own_construct) {
    Program program = parse_ok(
        "void f() {\n"
        "    while (1) {\n"
        "        switch (state) {\n"
        "            case 1: a(); break;\n"
        "            case 2: continue;\n"
        "        }\n"
        "        b();\n"
        "    }\n"
        "}\n");
    const Stmt* loop = body_of(program, "f")->body[0].get();
    CHECK(loop->kind == StmtKind::While);
    const Stmt* loop_body = loop->then_branch.get();
    CHECK_EQ(loop_body->body.size(), size_t(2));

    const Stmt* outer = loop_body->body[0].get();
    CHECK(outer->kind == StmtKind::Block);
    const Stmt* chain = outer->body[0].get();
    // The 'break' left the switch, so it must not survive as a loop break.
    const Stmt* first_branch = chain->then_branch.get();
    CHECK_EQ(first_branch->body.size(), size_t(1));
    CHECK(first_branch->body[0]->kind == StmtKind::Expression);
    // 'continue' means the same thing in both constructs and is kept.
    CHECK(chain->else_branch->then_branch->body[0]->kind == StmtKind::Continue);
    // The statement after the switch is untouched.
    const Stmt* after = loop_body->body[1].get();
    CHECK(after->kind == StmtKind::Expression);
    CHECK(render(after->expr.get()) == std::string("call b()"));
}

TEST(parser_switch_keeps_a_break_that_belongs_to_a_nested_loop) {
    Program program = parse_ok(
        "void f() {\n"
        "    switch (state) {\n"
        "        case 1:\n"
        "            while (1) { break; }\n"
        "            break;\n"
        "        case 2: b(); break;\n"
        "    }\n"
        "}\n");
    const Stmt* chain = lowered_switch(program)->body[0].get();
    const Stmt* branch = chain->then_branch.get();
    CHECK_EQ(branch->body.size(), size_t(1));
    const Stmt* inner_loop = branch->body[0].get();
    CHECK(inner_loop->kind == StmtKind::While);
    CHECK(inner_loop->then_branch->body[0]->kind == StmtKind::Break);
}

TEST(parser_nested_switches_use_distinct_temporaries) {
    Program program = parse_ok(
        "void f() {\n"
        "    switch (outerRead()) {\n"
        "        case 1:\n"
        "            switch (innerRead()) {\n"
        "                case 2: a(); break;\n"
        "            }\n"
        "            break;\n"
        "    }\n"
        "}\n");
    const Stmt* outer = lowered_switch(program);
    CHECK_EQ(outer->body.size(), size_t(2));
    const std::string outer_temp = outer->body[0]->var_name;
    CHECK(!outer_temp.empty());
    CHECK(render(outer->body[1]->expr.get()) == "(" + outer_temp + " == 1)");

    const Stmt* inner = outer->body[1]->then_branch->body[0].get();
    CHECK(inner->kind == StmtKind::Block);
    CHECK_EQ(inner->body.size(), size_t(2));
    const std::string inner_temp = inner->body[0]->var_name;
    CHECK(!inner_temp.empty());
    CHECK(outer_temp != inner_temp);
    CHECK(render(inner->body[0]->var_init.get()) == std::string("call innerRead()"));
    CHECK(render(inner->body[1]->expr.get()) == "(" + inner_temp + " == 2)");
}

TEST(parser_switch_rejects_fall_through_between_cases) {
    ParseResult result = parse_text(
        "void f() {\n"
        "    switch (state) {\n"
        "        case 1: a();\n"
        "        case 2: b(); break;\n"
        "    }\n"
        "}\n");
    CHECK(!result.ok);
    CHECK(result.error.rfind("line 3:", 0) == 0);
    CHECK(result.error.find("falls through") != std::string::npos);
}

TEST(parser_switch_rejects_fall_through_into_default) {
    ParseResult result = parse_text(
        "void f() {\n"
        "    switch (state) {\n"
        "        case 1: a();\n"
        "        default: d();\n"
        "    }\n"
        "}\n");
    CHECK(!result.ok);
    CHECK(result.error.find("falls through") != std::string::npos);
}

TEST(parser_switch_rejects_a_break_that_is_not_the_last_statement) {
    ParseResult result = parse_text(
        "void f() {\n"
        "    switch (state) {\n"
        "        case 1:\n"
        "            if (x) break;\n"
        "            a();\n"
        "            break;\n"
        "    }\n"
        "}\n");
    CHECK(!result.ok);
    CHECK(result.error.rfind("line 4:", 0) == 0);
    CHECK(result.error.find("'break' inside a switch") != std::string::npos);
}

TEST(parser_switch_rejects_a_duplicate_default) {
    ParseResult result = parse_text(
        "void f() { switch (state) { default: a(); break; default: b(); break; } }");
    CHECK(!result.ok);
    CHECK(result.error.find("duplicate 'default'") != std::string::npos);
}

TEST(parser_switch_rejects_a_statement_before_the_first_label) {
    ParseResult result = parse_text("void f() { switch (state) { a(); case 1: b(); } }");
    CHECK(!result.ok);
    CHECK(result.error.find("must follow a 'case'") != std::string::npos);
}

TEST(parser_case_label_outside_a_switch_is_an_error) {
    ParseResult result = parse_text("void f() { case 1: a(); }");
    CHECK(!result.ok);
    CHECK(result.error.find("outside of a switch") != std::string::npos);
}

TEST(parser_switch_requires_a_colon_after_a_label) {
    ParseResult result = parse_text("void f() { switch (state) { case 1 a(); } }");
    CHECK(!result.ok);
    CHECK(result.error.find("expected ':'") != std::string::npos);
}

TEST(parser_switch_state_machine_round_trip) {
    Program program = parse_ok(
        "enum Mode { Idle, Printing, Done };\n"
        "int mode;\n"
        "void loop() {\n"
        "    switch (mode) {\n"
        "        case Idle:\n"
        "            if (buttonPressed()) mode = Printing;\n"
        "            break;\n"
        "        case Printing:\n"
        "            for (int i = 0; i < 8; ++i) {\n"
        "                if (i > 4) break;\n"
        "                feed(i);\n"
        "            }\n"
        "            mode = Done;\n"
        "            break;\n"
        "        default:\n"
        "            mode = Idle;\n"
        "            break;\n"
        "    }\n"
        "}\n");
    const Stmt* outer = body_of(program, "loop")->body[0].get();
    CHECK(outer->kind == StmtKind::Block);
    const Stmt* chain = outer->body[0].get();
    CHECK(render(chain->expr.get()) == std::string("(mode == Idle)"));
    const Stmt* printing = chain->else_branch.get();
    CHECK(render(printing->expr.get()) == std::string("(mode == Printing)"));
    // The loop's own break is untouched inside the Printing branch.
    const Stmt* loop = printing->then_branch->body[0].get();
    CHECK(loop->kind == StmtKind::For);
    CHECK(loop->then_branch->body[0]->then_branch->kind == StmtKind::Break);
    CHECK(printing->else_branch->kind == StmtKind::Block);
}

// ------------------------------------------------------- enumerations -----

TEST(parser_enum_tag_names_a_usable_type) {
    Program program = parse_ok(
        "enum State { MainMenu, Printing, Done };\n"
        "State currentState = MainMenu;\n"
        "void run(State next) { State local = Printing; local = next; }\n");
    const Global* current = find_global(program, "currentState");
    CHECK(current != nullptr);
    CHECK(current->type->kind == TypeKind::Int);
    CHECK(render(current->init.get()) == std::string("MainMenu"));

    const Function* run = find_function(program, "run");
    CHECK(run != nullptr);
    CHECK_EQ(run->params.size(), size_t(1));
    CHECK(run->params[0].type->kind == TypeKind::Int);
    CHECK(run->params[0].name == std::string("next"));
    const Stmt* local = run->body->body[0].get();
    CHECK(local->kind == StmtKind::VarDecl);
    CHECK(local->var_type->kind == TypeKind::Int);
    CHECK(render(local->var_init.get()) == std::string("Printing"));
}

TEST(parser_scoped_enum_is_a_type_and_its_enumerators_are_globals) {
    Program program = parse_ok(
        "enum class Mode : unsigned char { Off, On };\n"
        "Mode mode = Mode::On;\n");
    const Global* off = find_global(program, "Off");
    CHECK(off != nullptr);
    CHECK_EQ(off->init->int_value, 0L);
    const Global* mode = find_global(program, "mode");
    CHECK(mode != nullptr);
    CHECK(mode->type->kind == TypeKind::Int);
    CHECK(render(mode->init.get()) == std::string("On"));
}

TEST(parser_enumerator_values_fold_constant_expressions) {
    Program program = parse_ok("enum Bits { None = 0, Low = 1 << 2, Both = Low | 1, Back = -3 };");
    CHECK_EQ(find_global(program, "Low")->init->int_value, 4L);
    CHECK_EQ(find_global(program, "Both")->init->int_value, 5L);
    CHECK_EQ(find_global(program, "Back")->init->int_value, -3L);
}

TEST(parser_enum_rejects_a_non_constant_enumerator) {
    ParseResult result = parse_text("int n; enum Bad { X = n + f() };");
    CHECK(!result.ok);
    CHECK(result.error.find("constant integer enumerator") != std::string::npos);
}

TEST(parser_enum_type_is_usable_in_a_cast_and_a_trailing_comma_is_allowed) {
    Program program = parse_ok(
        "enum State { A, B, };\n"
        "void f(int raw) { State s = (State)raw; s = B; }\n");
    CHECK_EQ(program.globals.size(), size_t(2));
    const Stmt* decl = find_function(program, "f")->body->body[0].get();
    CHECK(decl->kind == StmtKind::VarDecl);
    CHECK(render(decl->var_init.get()) == std::string("(cast raw)"));
}

// -------------------------------------------------- brace initialisers -----

TEST(parser_brace_initialiser_keeps_every_element) {
    Program program = parse_ok("int table[4] = { 1, 2, 3, 4 };");
    const Global* table = find_global(program, "table");
    CHECK(table != nullptr);
    CHECK(table->type->kind == TypeKind::Array);
    CHECK_EQ(table->type->array_length, 4L);
    CHECK(table->init->kind == ExprKind::InitList);
    CHECK(render(table->init.get()) == std::string("{1, 2, 3, 4}"));
}

TEST(parser_nested_brace_initialiser_nests) {
    Program program = parse_ok("int vector[2][3] = { {1, 2, 3}, {4, 5, 6} };");
    const Global* v = find_global(program, "vector");
    CHECK_EQ(v->type->array_length, 2L);
    CHECK_EQ(v->type->pointee->array_length, 3L);
    CHECK(render(v->init.get()) == std::string("{{1, 2, 3}, {4, 5, 6}}"));
}

TEST(parser_array_length_is_deduced_from_the_initialiser) {
    Program program = parse_ok(
        "int t[] = { 1, 2, 3 };\n"
        "char s[] = \"AB\";\n"
        "int rows[][2] = { {1, 2}, {3, 4}, {5, 6} };\n");
    CHECK_EQ(find_global(program, "t")->type->array_length, 3L);
    CHECK_EQ(find_global(program, "s")->type->array_length, 3L);
    const Global* rows = find_global(program, "rows");
    CHECK_EQ(rows->type->array_length, 3L);
    CHECK_EQ(rows->type->pointee->array_length, 2L);
}

TEST(parser_local_array_takes_a_brace_initialiser) {
    Program program = parse_ok("void f() { int a[] = { 7, 8 }; }");
    const Stmt* decl = body_of(program, "f")->body[0].get();
    CHECK(decl->kind == StmtKind::VarDecl);
    CHECK_EQ(decl->var_type->array_length, 2L);
    CHECK(render(decl->var_init.get()) == std::string("{7, 8}"));
}

TEST(parser_empty_brace_initialiser_is_kept) {
    Program program = parse_ok("int zeros[3] = {}; int n = {};");
    CHECK(find_global(program, "zeros")->init->kind == ExprKind::InitList);
    CHECK_EQ(find_global(program, "zeros")->init->args.size(), size_t(0));
    CHECK_EQ(find_global(program, "n")->init->int_value, 0L);
}

TEST(parser_scalar_brace_initialiser_unwraps_to_its_value) {
    Program program = parse_ok("int n = { 5 };");
    CHECK(find_global(program, "n")->init->kind == ExprKind::IntLiteral);
    CHECK_EQ(find_global(program, "n")->init->int_value, 5L);
}

TEST(parser_too_many_initialisers_is_an_error) {
    ParseResult result = parse_text("int table[2] = { 1, 2, 3 };");
    CHECK(!result.ok);
    CHECK(result.error.find("too many initialisers") != std::string::npos);

    ParseResult scalar = parse_text("int n = { 1, 2 };");
    CHECK(!scalar.ok);
    CHECK(scalar.error.find("too many initialisers for a scalar") != std::string::npos);

    ParseResult row = parse_text("int rows[2][2] = { {1, 2, 3}, {4, 5} };");
    CHECK(!row.ok);
    CHECK(row.error.find("too many initialisers") != std::string::npos);
}

TEST(parser_unterminated_brace_initialiser_is_an_error) {
    ParseResult result = parse_text("int t[] = { 1, 2");
    CHECK(!result.ok);
    CHECK(result.error.find("brace initialiser") != std::string::npos);
}

TEST(parser_font_table_survives_intact) {
    Program program = parse_ok(
        "int vector[3][4] = {\n"
        "  { 0, 1, 2, 3 },\n"
        "  { 4, 5, 6, 7 },\n"
        "  { 8, 9, 10, 11 },\n"
        "};\n");
    const Global* v = find_global(program, "vector");
    CHECK_EQ(v->init->args.size(), size_t(3));
    for (size_t i = 0; i < 3; ++i) CHECK_EQ(v->init->args[i]->args.size(), size_t(4));
    CHECK_EQ(v->init->args[2]->args[3]->int_value, 11L);
}
