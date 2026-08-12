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
