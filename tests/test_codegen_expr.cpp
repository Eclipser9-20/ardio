// Tests for AVR expression code generation.
//
// Each test builds a small AST by hand, runs it through CodeGen and asserts on
// the assembly text. A few tests additionally feed the generated text to the
// in-tree assembler, which is the real contract: whatever the code generator
// writes has to be encodable.

#include "harness.h"

#include "ardio/avr/assembler.h"
#include "ardio/avr/codegen.h"

using namespace ardio;

namespace {

TypePtr int_type() { return make_type(TypeKind::Int); }
TypePtr char_type() { return make_type(TypeKind::Char); }

ExprPtr literal(long v, TypePtr t = int_type()) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::IntLiteral;
    e->int_value = v;
    e->type = t;
    return e;
}

ExprPtr identifier(const std::string& name, TypePtr t = int_type()) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Identifier;
    e->name = name;
    e->type = t;
    return e;
}

ExprPtr binary(const std::string& op, ExprPtr a, ExprPtr b, TypePtr t = int_type()) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Binary;
    e->op = op;
    e->lhs = std::move(a);
    e->rhs = std::move(b);
    e->type = t;
    return e;
}

ExprPtr unary(const std::string& op, ExprPtr a, bool postfix = false,
              TypePtr t = int_type()) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Unary;
    e->op = op;
    e->lhs = std::move(a);
    e->is_postfix = postfix;
    e->type = t;
    return e;
}

ExprPtr assign(const std::string& op, ExprPtr a, ExprPtr b) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Assign;
    e->op = op;
    e->lhs = std::move(a);
    e->rhs = std::move(b);
    e->type = int_type();
    return e;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Runs one expression and hands back the assembly.
std::string gen(const Expr& e, CodeGen& cg) {
    cg.gen_expr(e);
    return cg.out;
}

} // namespace

TEST(codegen_int_literal) {
    CodeGen cg;
    std::string text = gen(*literal(0x1234), cg);
    CHECK(cg.error.empty());
    CHECK(text == "ldi r24, 52\nldi r25, 18\n");
}

TEST(codegen_negative_literal_uses_byte_values) {
    CodeGen cg;
    std::string text = gen(*literal(-1), cg);
    CHECK(text == "ldi r24, 255\nldi r25, 255\n");
}

TEST(codegen_local_identifier_uses_frame_pointer) {
    CodeGen cg;
    cg.set_local_offset("x", 4);
    std::string text = gen(*identifier("x"), cg);
    CHECK(text == "ldd r24, Y+4\nldd r25, Y+5\n");
}

TEST(codegen_global_identifier_uses_lds) {
    CodeGen cg;
    cg.set_global_address("counter", 0x0100);
    std::string text = gen(*identifier("counter"), cg);
    CHECK(text == "lds r24, 256\nlds r25, 257\n");
}

TEST(codegen_char_local_is_sign_extended) {
    CodeGen cg;
    cg.set_local_offset("c", 2);
    std::string text = gen(*identifier("c", char_type()), cg);
    CHECK(contains(text, "ldd r24, Y+2"));
    CHECK(!contains(text, "ldd r25"));
    CHECK(contains(text, "sbrc r24, 7"));
    CHECK(contains(text, "com r25"));
}

TEST(codegen_addition_16bit) {
    CodeGen cg;
    cg.set_local_offset("a", 0);
    cg.set_local_offset("b", 2);
    std::string text = gen(*binary("+", identifier("a"), identifier("b")), cg);
    CHECK(contains(text, "push r24"));
    CHECK(contains(text, "movw r22, r24"));
    CHECK(contains(text, "pop r24"));
    CHECK(contains(text, "add r24, r22"));
    CHECK(contains(text, "adc r25, r23"));
}

TEST(codegen_subtraction_uses_sub_sbc) {
    CodeGen cg;
    std::string text = gen(*binary("-", literal(9), literal(3)), cg);
    CHECK(contains(text, "sub r24, r22"));
    CHECK(contains(text, "sbc r25, r23"));
}

TEST(codegen_multiply_16bit_clears_r1) {
    CodeGen cg;
    std::string text = gen(*binary("*", literal(6), literal(7)), cg);
    CHECK(contains(text, "mul r24, r22"));
    CHECK(contains(text, "movw r18, r0"));
    CHECK(contains(text, "mul r25, r22"));
    CHECK(contains(text, "mul r24, r23"));
    CHECK(contains(text, "clr r1"));
    CHECK(contains(text, "movw r24, r18"));
}

TEST(codegen_multiply_8bit) {
    CodeGen cg;
    std::string text = gen(
        *binary("*", literal(3, char_type()), literal(4, char_type()), char_type()), cg);
    CHECK(contains(text, "mul r24, r22"));
    CHECK(contains(text, "mov r24, r0"));
    CHECK(contains(text, "clr r1"));
    CHECK(!contains(text, "movw r24, r18"));
}

TEST(codegen_bitwise_operators) {
    CodeGen cg;
    CHECK(contains(gen(*binary("&", literal(1), literal(2)), cg), "and r24, r22"));
    CodeGen cg2;
    CHECK(contains(gen(*binary("|", literal(1), literal(2)), cg2), "or r25, r23"));
    CodeGen cg3;
    CHECK(contains(gen(*binary("^", literal(1), literal(2)), cg3), "eor r24, r22"));
}

TEST(codegen_shift_left_loops) {
    CodeGen cg;
    std::string text = gen(*binary("<<", literal(1), literal(3)), cg);
    CHECK(contains(text, "tst r22"));
    CHECK(contains(text, "lsl r24"));
    CHECK(contains(text, "rol r25"));
    CHECK(contains(text, "dec r22"));
    CHECK(contains(text, "rjmp"));
}

TEST(codegen_shift_right_signed_uses_asr) {
    CodeGen cg;
    std::string text = gen(*binary(">>", literal(16), literal(2)), cg);
    CHECK(contains(text, "asr r25"));
    CHECK(contains(text, "ror r24"));
}

TEST(codegen_comparison_yields_zero_or_one) {
    CodeGen cg;
    std::string text = gen(*binary("==", literal(1), literal(2)), cg);
    CHECK(contains(text, "cp r24, r22"));
    CHECK(contains(text, "cpc r25, r23"));
    CHECK(contains(text, "ldi r24, 1"));
    CHECK(contains(text, "breq"));
    CHECK(contains(text, "ldi r24, 0"));
}

TEST(codegen_greater_than_swaps_operands) {
    CodeGen cg;
    std::string text = gen(*binary(">", literal(1), literal(2)), cg);
    CHECK(contains(text, "cp r22, r24"));
    CHECK(contains(text, "cpc r23, r25"));
    CHECK(contains(text, "brlt"));
}

TEST(codegen_unsigned_comparison_uses_brlo) {
    TypePtr u = make_type(TypeKind::UInt);
    CodeGen cg;
    std::string text = gen(*binary("<", literal(1, u), literal(2, u)), cg);
    CHECK(contains(text, "brlo"));
    CHECK(!contains(text, "brlt"));
}

TEST(codegen_logical_and_short_circuits) {
    CodeGen cg;
    std::string text = gen(*binary("&&", literal(1), literal(0)), cg);
    CHECK(contains(text, "or r24, r25"));
    CHECK(contains(text, "breq"));
    // The right operand is generated after the first branch, not before it.
    size_t first_branch = text.find("breq");
    size_t second_load = text.find("ldi r24, 0", first_branch);
    CHECK(first_branch != std::string::npos);
    CHECK(second_load != std::string::npos);
}

TEST(codegen_logical_or_short_circuits) {
    CodeGen cg;
    std::string text = gen(*binary("||", literal(0), literal(1)), cg);
    CHECK(contains(text, "brne"));
    CHECK(contains(text, "ldi r25, 0"));
}

TEST(codegen_unary_negate_16bit) {
    CodeGen cg;
    std::string text = gen(*unary("-", literal(5)), cg);
    CHECK(contains(text, "com r25"));
    CHECK(contains(text, "neg r24"));
    CHECK(contains(text, "sbci r25, -1"));
}

TEST(codegen_unary_not) {
    CodeGen cg;
    std::string text = gen(*unary("!", literal(5)), cg);
    CHECK(contains(text, "or r24, r25"));
    CHECK(contains(text, "brne"));
    CHECK(contains(text, "ldi r24, 1"));
}

TEST(codegen_unary_complement) {
    CodeGen cg;
    std::string text = gen(*unary("~", literal(5)), cg);
    CHECK(text == "ldi r24, 5\nldi r25, 0\ncom r24\ncom r25\n");
}

TEST(codegen_pre_increment_stores_new_value) {
    CodeGen cg;
    cg.set_local_offset("i", 0);
    std::string text = gen(*unary("++", identifier("i")), cg);
    CHECK(text == "ldd r24, Y+0\nldd r25, Y+1\nadiw r24, 1\n"
                  "std Y+0, r24\nstd Y+1, r25\n");
}

TEST(codegen_post_increment_keeps_old_value) {
    CodeGen cg;
    cg.set_local_offset("i", 0);
    std::string text = gen(*unary("++", identifier("i"), true), cg);
    CHECK(contains(text, "push r24"));
    CHECK(contains(text, "adiw r24, 1"));
    CHECK(contains(text, "pop r24"));
}

TEST(codegen_pre_decrement_uses_sbiw) {
    CodeGen cg;
    cg.set_local_offset("i", 0);
    std::string text = gen(*unary("--", identifier("i")), cg);
    CHECK(contains(text, "sbiw r24, 1"));
}

TEST(codegen_assignment_to_local) {
    CodeGen cg;
    cg.set_local_offset("x", 6);
    std::string text = gen(*assign("=", identifier("x"), literal(7)), cg);
    CHECK(text == "ldi r24, 7\nldi r25, 0\nstd Y+6, r24\nstd Y+7, r25\n");
}

TEST(codegen_assignment_to_global) {
    CodeGen cg;
    cg.set_global_address("g", 0x0200);
    std::string text = gen(*assign("=", identifier("g"), literal(1)), cg);
    CHECK(contains(text, "sts 512, r24"));
    CHECK(contains(text, "sts 513, r25"));
}

TEST(codegen_compound_assignment) {
    CodeGen cg;
    cg.set_local_offset("x", 0);
    std::string text = gen(*assign("+=", identifier("x"), literal(1)), cg);
    CHECK(contains(text, "ldd r24, Y+0"));
    CHECK(contains(text, "add r24, r22"));
    CHECK(contains(text, "std Y+0, r24"));
}

TEST(codegen_call_marshals_abi_registers) {
    CodeGen cg;
    auto call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->name = "digitalWrite";
    call->type = int_type();
    call->args.push_back(literal(13));
    call->args.push_back(literal(1));
    std::string text = gen(*call, cg);
    CHECK(cg.error.empty());
    // First argument in r24:r25, second in r22:r23, popped in reverse order.
    CHECK(contains(text, "pop r23\npop r22\npop r25\npop r24\n"));
    CHECK(contains(text, "call digitalWrite"));
}

TEST(codegen_call_third_argument_goes_to_r20) {
    CodeGen cg;
    auto call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->name = "three";
    call->type = int_type();
    call->args.push_back(literal(1));
    call->args.push_back(literal(2));
    call->args.push_back(literal(3));
    std::string text = gen(*call, cg);
    CHECK(contains(text, "pop r21"));
    CHECK(contains(text, "pop r20"));
}

TEST(codegen_conditional_expression) {
    CodeGen cg;
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Conditional;
    e->type = int_type();
    e->lhs = literal(1);
    e->rhs = literal(2);
    e->third = literal(3);
    std::string text = gen(*e, cg);
    CHECK(contains(text, "breq"));
    CHECK(contains(text, "rjmp"));
}

TEST(codegen_string_literal_is_rejected) {
    CodeGen cg;
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::StringLiteral;
    e->str_value = "hi";
    cg.gen_expr(*e);
    CHECK(!cg.error.empty());
}

TEST(codegen_unknown_operator_is_rejected) {
    CodeGen cg;
    cg.gen_expr(*binary("@", literal(1), literal(2)));
    CHECK(!cg.error.empty());
}

// ---- the generated text has to assemble ------------------------------------

TEST(codegen_output_assembles) {
    CodeGen cg;
    cg.set_local_offset("a", 0);
    cg.set_global_address("g", 0x0100);
    cg.gen_expr(*binary("+", identifier("a"),
                        binary("*", identifier("g"), literal(3))));
    CHECK(cg.error.empty());
    AssembleResult r = assemble(cg.out);
    CHECK(r.error.empty());
    CHECK(r.ok);
}

TEST(codegen_comparison_output_assembles) {
    CodeGen cg;
    cg.gen_expr(*binary("<=", literal(1), literal(2)));
    AssembleResult r = assemble(cg.out);
    CHECK(r.error.empty());
    CHECK(r.ok);
}

TEST(codegen_shift_output_assembles) {
    CodeGen cg;
    cg.gen_expr(*binary("<<", literal(1), literal(4)));
    AssembleResult r = assemble(cg.out);
    CHECK(r.error.empty());
    CHECK(r.ok);
}

TEST(codegen_logical_output_assembles) {
    CodeGen cg;
    cg.gen_expr(*binary("&&", literal(1), binary("||", literal(0), literal(2))));
    AssembleResult r = assemble(cg.out);
    CHECK(r.error.empty());
    CHECK(r.ok);
}

TEST(codegen_call_output_assembles) {
    CodeGen cg;
    auto call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->name = "f";
    call->type = int_type();
    call->args.push_back(literal(1));
    cg.gen_expr(*call);
    cg.emit("f:");
    cg.emit("ret");
    AssembleResult r = assemble(cg.out);
    CHECK(r.error.empty());
    CHECK(r.ok);
}
