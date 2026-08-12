// Tests for AVR expression code generation.
//
// Each test builds a small AST by hand, runs it through CodeGen and asserts on
// the assembly text. A few tests additionally feed the generated text to the
// in-tree assembler, which is the real contract: whatever the code generator
// writes has to be encodable.

#include "harness.h"

#include "ardio/avr/assembler.h"
#include "ardio/avr/codegen.h"
#include "ardio/avr/sema.h"

#include <fstream>
#include <sstream>

namespace ardio {
// Field offsets reach the code generator through a table beside it, the same
// way class sizes reach Type::size(). The generator owns these; they are
// declared here rather than in codegen.h so the header keeps its current
// shape.
void set_class_layout(const ClassDecl& c);
int  class_field_offset(const std::string& class_name, const std::string& field);
void clear_class_layouts();
} // namespace ardio

using namespace ardio;

namespace {

TypePtr int_type() { return make_type(TypeKind::Int); }
TypePtr char_type() { return make_type(TypeKind::Char); }
TypePtr uint_type() { return make_type(TypeKind::UInt); }

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

ExprPtr index(ExprPtr base, ExprPtr subscript, TypePtr element = int_type()) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Index;
    e->lhs = std::move(base);
    e->rhs = std::move(subscript);
    e->type = element;                       // the element type, as sema fills it
    return e;
}

ExprPtr member(ExprPtr object, const std::string& field, bool through_pointer,
               TypePtr t = int_type()) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Member;
    e->lhs = std::move(object);
    e->name = field;
    e->through_pointer = through_pointer;
    e->type = t;
    return e;
}

ExprPtr string_literal(const std::string& text) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::StringLiteral;
    e->str_value = text;
    e->type = make_array(char_type(), static_cast<long>(text.size()) + 1);
    return e;
}

// Runs one expression and hands back the assembly.
std::string gen(const Expr& e, CodeGen& cg) {
    cg.gen_expr(e);
    return cg.out;
}

// The tests below assemble what they generate, which for division means the
// helper routines have to be there too. The test binary may run from the build
// directory or from the source root.
bool read_runtime(const char* relative, std::string& out) {
    static const char* const prefixes[] = {"", "../", "../../", "../../../"};
    for (const char* prefix : prefixes) {
        std::ifstream in(std::string(prefix) + relative, std::ios::binary);
        if (!in) continue;
        std::ostringstream buf;
        buf << in.rdbuf();
        out = buf.str();
        return true;
    }
    return false;
}

// Assembles `text`, appending runtime/math.S so calls into it resolve. Returns
// false only when the assembler rejects the result; a missing runtime file
// makes the caller skip instead.
bool assembles_with_math(const std::string& text, std::string& error) {
    std::string math;
    if (!read_runtime("runtime/math.S", math)) {
        std::printf("  skip runtime/math.S not found relative to the cwd\n");
        return true;
    }
    AssembleResult r = assemble(text + "\nrjmp __ardio_math_end\n" + math +
                                "\n__ardio_math_end:\n");
    error = r.error;
    return r.ok;
}

// A two-field class the member tests share: { char flag; int value; }.
ClassDecl point_class() {
    ClassDecl c;
    c.name = "Point";
    c.fields.push_back({"flag", char_type(), 0});
    c.fields.push_back({"value", int_type(), 1});
    c.size = 3;
    set_class_size(c.name, c.size);
    return c;
}

TypePtr point_type() {
    auto t = make_type(TypeKind::Class);
    t->class_name = "Point";
    return t;
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

// ---- division and modulo ----------------------------------------------------

TEST(codegen_division_calls_the_signed_helper) {
    CodeGen cg;
    std::string text = gen(*binary("/", literal(100), literal(7)), cg);
    CHECK(cg.error.empty());
    // The operands are already where the helper wants them.
    CHECK(contains(text, "movw r22, r24"));
    CHECK(contains(text, "call __ardio_divmod16"));
    CHECK(!contains(text, "movw r24, r18"));      // '/' keeps the quotient
}

TEST(codegen_modulo_takes_the_remainder) {
    CodeGen cg;
    std::string text = gen(*binary("%", literal(100), literal(7)), cg);
    CHECK(contains(text, "call __ardio_divmod16"));
    CHECK(contains(text, "movw r24, r18"));
}

TEST(codegen_unsigned_division_calls_the_unsigned_helper) {
    CodeGen cg;
    std::string text = gen(*binary("/", literal(100, uint_type()),
                                   literal(7, uint_type()), uint_type()), cg);
    CHECK(contains(text, "call __ardio_udivmod16"));
    CHECK(!contains(text, "call __ardio_divmod16"));
}

TEST(codegen_char_division_rewidens_the_result) {
    CodeGen cg;
    std::string text = gen(*binary("/", literal(9, char_type()),
                                   literal(2, char_type()), char_type()), cg);
    CHECK(contains(text, "call __ardio_divmod16"));
    CHECK(contains(text, "sbrc r24, 7"));         // sign-extend back to 16 bits
}

TEST(codegen_division_output_assembles) {
    CodeGen cg;
    cg.gen_expr(*binary("%", binary("/", literal(1000), literal(3)), literal(7)));
    CHECK(cg.error.empty());
    std::string error;
    bool ok = assembles_with_math(cg.out, error);
    if (!ok) std::printf("    assembler said: %s\n", error.c_str());
    CHECK(ok);
}

TEST(runtime_math_assembles_on_its_own) {
    std::string math;
    if (!read_runtime("runtime/math.S", math)) {
        std::printf("  skip runtime/math.S not found relative to the cwd\n");
        return;
    }
    AssembleResult r = assemble(math);
    if (!r.ok) std::printf("    assembler said: %s\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(!r.code.empty());
    CHECK_EQ(r.code.size() % 2, size_t(0));
}

// ---- subscripts -------------------------------------------------------------

TEST(codegen_index_of_local_array_scales_by_element_size) {
    CodeGen cg;
    cg.set_local_offset("a", 4);
    TypePtr array = make_array(int_type(), 8);
    std::string text = gen(*index(identifier("a", array), literal(2)), cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "movw r24, r28"));       // the frame pointer
    CHECK(contains(text, "adiw r24, 4"));         // ... plus the array's slot
    CHECK(contains(text, "lsl r24"));             // index * 2
    CHECK(contains(text, "rol r25"));
    CHECK(contains(text, "add r24, r22"));
    CHECK(contains(text, "movw r30, r24"));       // read through Z
    CHECK(contains(text, "ld r24, Z"));
    CHECK(contains(text, "ldd r25, Z+1"));
}

TEST(codegen_index_of_char_array_does_not_scale) {
    CodeGen cg;
    cg.set_global_address("buf", 0x0300);
    TypePtr array = make_array(char_type(), 16);
    std::string text = gen(*index(identifier("buf", array), identifier("i"),
                                  char_type()), cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "ldi r24, 0"));          // low byte of 0x0300
    CHECK(contains(text, "ldi r25, 3"));          // high byte
    CHECK(!contains(text, "lsl r24"));
    CHECK(contains(text, "ld r24, Z"));
    CHECK(!contains(text, "ldd r25, Z+1"));       // one byte only
    CHECK(contains(text, "sbrc r24, 7"));         // sign-extended for the caller
}

TEST(codegen_index_through_a_pointer_loads_the_pointer) {
    CodeGen cg;
    cg.set_local_offset("p", 2);
    TypePtr pointer = make_pointer(int_type());
    std::string text = gen(*index(identifier("p", pointer), literal(1)), cg);
    CHECK(cg.error.empty());
    // A pointer's value is the base address; no frame arithmetic at all.
    CHECK(contains(text, "ldd r24, Y+2"));
    CHECK(!contains(text, "movw r24, r28"));
}

TEST(codegen_index_with_odd_element_size_multiplies) {
    CodeGen cg;
    clear_class_layouts();
    ClassDecl c = point_class();
    set_class_layout(c);
    cg.set_local_offset("pts", 1);
    TypePtr array = make_array(point_type(), 4);
    // pts[2].value -- an array of three-byte objects, so the subscript is
    // scaled by MUL rather than by shifts.
    std::string text = gen(*member(index(identifier("pts", array), literal(2),
                                         point_type()), "value", false), cg);
    CHECK(cg.error.empty());
    CHECK_EQ(c.size, 3);
    CHECK(contains(text, "ldi r22, 3"));
    CHECK(contains(text, "mul r24, r22"));
    CHECK(contains(text, "clr r1"));              // the zero register is restored
    CHECK(contains(text, "adiw r24, 1"));         // the field within the element
    CHECK(contains(text, "ld r24, Z"));
}

TEST(codegen_assignment_to_array_element) {
    CodeGen cg;
    cg.set_local_offset("a", 0);
    TypePtr array = make_array(int_type(), 4);
    auto e = assign("=", index(identifier("a", array), literal(3)), literal(9));
    std::string text = gen(*e, cg);
    CHECK(cg.error.empty());
    // The value is computed first and parked while the address is worked out.
    CHECK(contains(text, "ldi r24, 9"));
    CHECK(contains(text, "movw r30, r24"));
    CHECK(contains(text, "pop r25\npop r24\nst Z, r24\nstd Z+1, r25\n"));
}

TEST(codegen_array_name_decays_to_its_address) {
    CodeGen cg;
    cg.set_local_offset("a", 6);
    TypePtr array = make_array(int_type(), 4);
    std::string text = gen(*identifier("a", array), cg);
    CHECK(text == "movw r24, r28\nadiw r24, 6\n");
}

TEST(codegen_index_output_assembles) {
    CodeGen cg;
    cg.set_local_offset("a", 2);
    cg.set_local_offset("i", 10);
    TypePtr array = make_array(int_type(), 8);
    cg.gen_expr(*assign("=", index(identifier("a", array), identifier("i")),
                        index(identifier("a", array), literal(0))));
    CHECK(cg.error.empty());
    AssembleResult r = assemble(cg.out);
    if (!r.ok) std::printf("    assembler said: %s\n", r.error.c_str());
    CHECK(r.ok);
}

// ---- address-of and dereference ---------------------------------------------

TEST(codegen_address_of_local) {
    CodeGen cg;
    cg.set_local_offset("x", 5);
    std::string text = gen(*unary("&", identifier("x"), false,
                                  make_pointer(int_type())), cg);
    CHECK(text == "movw r24, r28\nadiw r24, 5\n");
}

TEST(codegen_address_of_global) {
    CodeGen cg;
    cg.set_global_address("g", 0x0104);
    std::string text = gen(*unary("&", identifier("g"), false,
                                  make_pointer(int_type())), cg);
    CHECK(text == "ldi r24, 4\nldi r25, 1\n");
}

TEST(codegen_dereference_reads_through_z) {
    CodeGen cg;
    cg.set_local_offset("p", 0);
    auto e = unary("*", identifier("p", make_pointer(int_type())));
    std::string text = gen(*e, cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "ldd r24, Y+0"));
    CHECK(contains(text, "movw r30, r24"));
    CHECK(contains(text, "ld r24, Z"));
    CHECK(contains(text, "ldd r25, Z+1"));
}

TEST(codegen_assignment_through_a_dereference) {
    CodeGen cg;
    cg.set_local_offset("p", 0);
    auto target = unary("*", identifier("p", make_pointer(char_type())), false,
                        char_type());
    std::string text = gen(*assign("=", std::move(target), literal(65)), cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "st Z, r24"));
    CHECK(!contains(text, "std Z+1, r25"));       // a char is one byte
}

TEST(codegen_address_of_a_literal_is_rejected) {
    CodeGen cg;
    cg.gen_expr(*unary("&", literal(7), false, make_pointer(int_type())));
    CHECK(!cg.error.empty());
}

TEST(codegen_pointer_output_assembles) {
    CodeGen cg;
    cg.set_local_offset("x", 1);
    cg.set_local_offset("p", 3);
    cg.gen_expr(*assign("=", unary("*", identifier("p", make_pointer(int_type()))),
                        unary("&", identifier("x"), false,
                              make_pointer(int_type()))));
    CHECK(cg.error.empty());
    AssembleResult r = assemble(cg.out);
    if (!r.ok) std::printf("    assembler said: %s\n", r.error.c_str());
    CHECK(r.ok);
}

// ---- members ----------------------------------------------------------------

TEST(codegen_member_of_a_local_object) {
    CodeGen cg;
    clear_class_layouts();
    set_class_layout(point_class());
    cg.set_local_offset("p", 4);
    std::string text = gen(*member(identifier("p", point_type()), "value", false), cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "movw r24, r28"));
    CHECK(contains(text, "adiw r24, 4"));         // the object
    CHECK(contains(text, "adiw r24, 1"));         // the field's offset
    CHECK(contains(text, "ld r24, Z"));
}

TEST(codegen_member_through_a_pointer_uses_the_pointer_value) {
    CodeGen cg;
    clear_class_layouts();
    set_class_layout(point_class());
    cg.set_local_offset("p", 0);
    std::string text = gen(*member(identifier("p", make_pointer(point_type())),
                                   "value", true), cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "ldd r24, Y+0"));
    CHECK(!contains(text, "movw r24, r28"));
    CHECK(contains(text, "adiw r24, 1"));
}

TEST(codegen_first_field_needs_no_offset) {
    CodeGen cg;
    clear_class_layouts();
    set_class_layout(point_class());
    cg.set_local_offset("p", 0);
    std::string text = gen(*member(identifier("p", point_type()), "flag", false,
                                   char_type()), cg);
    CHECK(contains(text, "movw r24, r28"));
    CHECK(!contains(text, "adiw r24"));
    CHECK_EQ(class_field_offset("Point", "flag"), 0);
    CHECK_EQ(class_field_offset("Point", "value"), 1);
}

TEST(codegen_assignment_to_a_member) {
    CodeGen cg;
    clear_class_layouts();
    set_class_layout(point_class());
    cg.set_local_offset("p", 2);
    std::string text = gen(*assign("=", member(identifier("p", point_type()),
                                               "value", false), literal(12)), cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "ldi r24, 12"));
    CHECK(contains(text, "st Z, r24"));
    CHECK(contains(text, "std Z+1, r25"));
}

TEST(codegen_unknown_member_is_rejected) {
    CodeGen cg;
    clear_class_layouts();
    set_class_layout(point_class());
    cg.set_local_offset("p", 0);
    cg.gen_expr(*member(identifier("p", point_type()), "missing", false));
    CHECK(!cg.error.empty());
    CHECK_EQ(class_field_offset("Point", "missing"), -1);
}

TEST(codegen_member_output_assembles) {
    CodeGen cg;
    clear_class_layouts();
    set_class_layout(point_class());
    cg.set_local_offset("p", 1);
    cg.gen_expr(*assign("=", member(identifier("p", point_type()), "value", false),
                        member(identifier("p", point_type()), "flag", false,
                               char_type())));
    CHECK(cg.error.empty());
    AssembleResult r = assemble(cg.out);
    if (!r.ok) std::printf("    assembler said: %s\n", r.error.c_str());
    CHECK(r.ok);
}

// ---- string literals --------------------------------------------------------

TEST(codegen_string_literal_yields_an_address) {
    CodeGen cg;
    cg.next_global_address = 0x0200;
    std::string text = gen(*string_literal("hi"), cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "ldi r30, 0\nldi r31, 2\n"));   // Z = 0x0200
    CHECK(contains(text, "ldi r18, 104\nst Z+, r18\n")); // 'h'
    CHECK(contains(text, "ldi r18, 105\nst Z+, r18\n")); // 'i'
    CHECK(contains(text, "ldi r18, 0\nst Z+, r18\n"));   // the terminator
    CHECK(contains(text, "ldi r24, 0\nldi r25, 2\n"));   // the value: 0x0200
}

TEST(codegen_equal_string_literals_share_storage) {
    CodeGen cg;
    cg.next_global_address = 0x0200;
    cg.gen_expr(*string_literal("ab"));
    cg.gen_expr(*string_literal("ab"));
    cg.gen_expr(*string_literal("cd"));
    CHECK(cg.error.empty());
    // "ab" is three bytes, so the second distinct literal starts after it.
    CHECK_EQ(cg.next_global_address, 0x0206);
}

TEST(codegen_string_literal_as_a_call_argument_assembles) {
    CodeGen cg;
    auto call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->name = "print";
    call->type = int_type();
    call->args.push_back(string_literal("hello, world"));
    cg.gen_expr(*call);
    cg.emit("print:");
    cg.emit("ret");
    CHECK(cg.error.empty());
    AssembleResult r = assemble(cg.out);
    if (!r.ok) std::printf("    assembler said: %s\n", r.error.c_str());
    CHECK(r.ok);
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

// ============================ 32-bit code generation =========================
//
// A `long` lives in r22..r25, which is where the ABI puts a 32-bit argument and
// a 32-bit return value. Checking that the right instructions appear is worth
// something, but it is weak evidence: an off-by-one in a carry chain looks
// exactly as plausible as the correct version. So the tests below assemble the
// generated code and then *run* it on the small interpreter in namespace `sim`,
// which decodes the machine words the assembler produced. That covers three
// things at once -- the code generator, the assembler's encoding, and the
// arithmetic itself.

namespace sim {

// An ATmega-shaped machine: 32 registers, 2K of SRAM, and the handful of
// instructions ardio's code generator and runtime actually emit. Anything else
// stops the run and is reported, so an untested encoding can never be mistaken
// for a passing test.
struct Cpu {
    uint8_t r[32] = {};
    uint8_t mem[0x0900] = {};
    uint16_t pc = 0;                 // word address
    uint16_t sp = 0x08FF;
    bool C = false, Z = false, N = false, V = false, T = false;
    bool stopped = false;
    std::string error;

    const std::vector<uint8_t>* image = nullptr;

    uint16_t word(uint16_t w) const {
        size_t i = size_t(w) * 2;
        if (i + 1 >= image->size()) return 0xFFFF;
        return uint16_t((*image)[i] | ((*image)[i + 1] << 8));
    }
    uint8_t  load(uint16_t a) const { return a < sizeof(mem) ? mem[a] : 0; }
    void     store(uint16_t a, uint8_t v) { if (a < sizeof(mem)) mem[a] = v; }
    void     push(uint8_t v) { store(sp--, v); }
    uint8_t  pop() { return load(++sp); }

    bool S() const { return N != V; }

    void logic_flags(uint8_t res) {
        V = false; N = (res & 0x80) != 0; Z = res == 0;
    }
    void add_flags(uint8_t a, uint8_t b, unsigned res, bool carry_in_zero) {
        C = res > 0xFF;
        uint8_t r8 = uint8_t(res);
        N = (r8 & 0x80) != 0;
        V = (((a ^ b) & 0x80) == 0) && (((a ^ r8) & 0x80) != 0);
        if (carry_in_zero) Z = r8 == 0; else Z = Z && r8 == 0;
    }
    void sub_flags(uint8_t a, uint8_t b, unsigned res, bool plain) {
        C = res > 0xFF;                       // borrow, encoded as a carry out
        uint8_t r8 = uint8_t(res);
        N = (r8 & 0x80) != 0;
        V = (((a ^ b) & 0x80) != 0) && (((a ^ r8) & 0x80) != 0);
        if (plain) Z = r8 == 0; else Z = Z && r8 == 0;
    }
};

// Decodes and runs one instruction. Returns false once the machine has
// stopped, either by returning past the sentinel or on an unknown opcode.
bool step(Cpu& c) {
    if (c.stopped) return false;
    uint16_t op = c.word(c.pc);
    uint16_t here = c.pc;
    c.pc++;

    auto rd5 = [&] { return (op >> 4) & 0x1F; };
    auto rr5 = [&] { return unsigned(((op >> 5) & 0x10) | (op & 0x0F)); };
    auto rd4 = [&] { return 16 + ((op >> 4) & 0x0F); };
    auto k8  = [&] { return uint8_t(((op >> 4) & 0xF0) | (op & 0x0F)); };

    // Two-operand arithmetic and logic: 0000..0010 blocks.
    switch (op & 0xFC00) {
    case 0x0C00: { // add
        unsigned res = c.r[rd5()] + c.r[rr5()];
        c.add_flags(c.r[rd5()], c.r[rr5()], res, true);
        c.r[rd5()] = uint8_t(res); return true;
    }
    case 0x1C00: { // adc
        unsigned res = c.r[rd5()] + c.r[rr5()] + (c.C ? 1 : 0);
        c.add_flags(c.r[rd5()], c.r[rr5()], res, false);
        c.r[rd5()] = uint8_t(res); return true;
    }
    case 0x1800: { // sub
        unsigned res = unsigned(c.r[rd5()]) - c.r[rr5()];
        c.sub_flags(c.r[rd5()], c.r[rr5()], res, true);
        c.C = c.r[rd5()] < c.r[rr5()];
        c.r[rd5()] = uint8_t(res); return true;
    }
    case 0x0800: { // sbc
        unsigned borrow = c.C ? 1u : 0u;
        unsigned res = unsigned(c.r[rd5()]) - c.r[rr5()] - borrow;
        bool new_c = unsigned(c.r[rd5()]) < unsigned(c.r[rr5()]) + borrow;
        c.sub_flags(c.r[rd5()], c.r[rr5()], res, false);
        c.C = new_c;
        c.r[rd5()] = uint8_t(res); return true;
    }
    case 0x2000: { // and
        uint8_t res = uint8_t(c.r[rd5()] & c.r[rr5()]);
        c.logic_flags(res); c.r[rd5()] = res; return true;
    }
    case 0x2400: { // eor
        uint8_t res = uint8_t(c.r[rd5()] ^ c.r[rr5()]);
        c.logic_flags(res); c.r[rd5()] = res; return true;
    }
    case 0x2800: { // or
        uint8_t res = uint8_t(c.r[rd5()] | c.r[rr5()]);
        c.logic_flags(res); c.r[rd5()] = res; return true;
    }
    case 0x1400: { // cp
        unsigned res = unsigned(c.r[rd5()]) - c.r[rr5()];
        c.sub_flags(c.r[rd5()], c.r[rr5()], res, true);
        c.C = c.r[rd5()] < c.r[rr5()];
        return true;
    }
    case 0x0400: { // cpc
        unsigned borrow = c.C ? 1u : 0u;
        unsigned res = unsigned(c.r[rd5()]) - c.r[rr5()] - borrow;
        bool new_c = unsigned(c.r[rd5()]) < unsigned(c.r[rr5()]) + borrow;
        c.sub_flags(c.r[rd5()], c.r[rr5()], res, false);
        c.C = new_c;
        return true;
    }
    case 0x2C00: c.r[rd5()] = c.r[rr5()]; return true;         // mov
    case 0x9C00: {                                             // mul
        unsigned p = unsigned(c.r[rd5()]) * c.r[rr5()];
        c.r[0] = uint8_t(p); c.r[1] = uint8_t(p >> 8);
        c.C = (p & 0x8000) != 0; c.Z = p == 0; return true;
    }
    default: break;
    }

    // Register-immediate: 0011..0111.
    switch (op & 0xF000) {
    case 0x3000: { // cpi
        unsigned res = unsigned(c.r[rd4()]) - k8();
        c.sub_flags(c.r[rd4()], k8(), res, true);
        c.C = c.r[rd4()] < k8();
        return true;
    }
    case 0x4000: { // sbci
        unsigned borrow = c.C ? 1u : 0u;
        unsigned res = unsigned(c.r[rd4()]) - k8() - borrow;
        bool new_c = unsigned(c.r[rd4()]) < unsigned(k8()) + borrow;
        c.sub_flags(c.r[rd4()], k8(), res, false);
        c.C = new_c;
        c.r[rd4()] = uint8_t(res); return true;
    }
    case 0x5000: { // subi
        unsigned res = unsigned(c.r[rd4()]) - k8();
        c.sub_flags(c.r[rd4()], k8(), res, true);
        c.C = c.r[rd4()] < k8();
        c.r[rd4()] = uint8_t(res); return true;
    }
    case 0x6000: { uint8_t v = uint8_t(c.r[rd4()] | k8());     // ori
                   c.logic_flags(v); c.r[rd4()] = v; return true; }
    case 0x7000: { uint8_t v = uint8_t(c.r[rd4()] & k8());     // andi
                   c.logic_flags(v); c.r[rd4()] = v; return true; }
    case 0xE000: c.r[rd4()] = k8(); return true;               // ldi
    case 0xC000: {                                             // rjmp
        int16_t k = int16_t(op & 0x0FFF);
        if (k & 0x0800) k = int16_t(k | int16_t(0xF000));
        c.pc = uint16_t(here + 1 + k); return true;
    }
    default: break;
    }

    if ((op & 0xFF00) == 0x0100) {                             // movw
        unsigned d = ((op >> 4) & 0x0F) * 2, r = (op & 0x0F) * 2;
        c.r[d] = c.r[r]; c.r[d + 1] = c.r[r + 1]; return true;
    }
    if ((op & 0xFE00) == 0x9600) {                             // adiw / sbiw
        static const unsigned pair[4] = {24, 26, 28, 30};
        unsigned d = pair[(op >> 4) & 3];
        unsigned k = ((op >> 2) & 0x30) | (op & 0x0F);
        unsigned v = unsigned(c.r[d]) | (unsigned(c.r[d + 1]) << 8);
        unsigned res = (op & 0x0100) ? v - k : v + k;
        c.r[d] = uint8_t(res); c.r[d + 1] = uint8_t(res >> 8);
        c.Z = (res & 0xFFFF) == 0; c.N = (res & 0x8000) != 0;
        c.C = (op & 0x0100) ? v < k : res > 0xFFFF; c.V = false;
        return true;
    }
    if ((op & 0xFC00) == 0xF400 || (op & 0xFC00) == 0xF000) {  // brbs / brbc
        unsigned bit = op & 7;
        bool set = (op & 0x0400) == 0;
        bool flag = bit == 0 ? c.C : bit == 1 ? c.Z : bit == 2 ? c.N
                  : bit == 3 ? c.V : bit == 4 ? c.S() : false;
        if (flag == set) {
            int16_t k = int16_t((op >> 3) & 0x7F);
            if (k & 0x40) k = int16_t(k | int16_t(0xFF80));
            c.pc = uint16_t(here + 1 + k);
        }
        return true;
    }
    if ((op & 0xFC08) == 0xFC00) {                             // sbrc / sbrs
        unsigned r = (op >> 4) & 0x1F, b = op & 7;
        bool bit = (c.r[r] >> b) & 1;
        bool skip_when_set = (op & 0x0200) != 0;
        if (bit == skip_when_set) {
            uint16_t next = c.word(c.pc);
            bool two = (next & 0xFE0E) == 0x940C || (next & 0xFE0E) == 0x940E ||
                       (next & 0xFE0F) == 0x9000 || (next & 0xFE0F) == 0x9200;
            c.pc = uint16_t(c.pc + (two ? 2 : 1));
        }
        return true;
    }
    if ((op & 0xFE0F) == 0x900F) { c.r[rd5()] = c.pop(); return true; }   // pop
    if ((op & 0xFE0F) == 0x920F) { c.push(c.r[rd5()]); return true; }     // push
    if ((op & 0xFE0F) == 0x9000) {                                       // lds
        uint16_t a = c.word(c.pc); c.pc++;
        c.r[rd5()] = c.load(a); return true;
    }
    if ((op & 0xFE0F) == 0x9200) {                                       // sts
        uint16_t a = c.word(c.pc); c.pc++;
        c.store(a, c.r[rd5()]); return true;
    }
    if ((op & 0xFE0E) == 0x940E) {                                       // call
        uint16_t target = c.word(c.pc); c.pc++;
        c.push(uint8_t(c.pc & 0xFF)); c.push(uint8_t(c.pc >> 8));
        c.pc = target; return true;
    }
    if (op == 0x9508) {                                                  // ret
        uint8_t hi = c.pop(), lo = c.pop();
        uint16_t target = uint16_t((hi << 8) | lo);
        if (target == 0xFFFF) { c.stopped = true; return false; }        // sentinel
        c.pc = target; return true;
    }
    if ((op & 0xFE0F) == 0x9400) {                             // com
        uint8_t v = uint8_t(~c.r[rd5()]);
        c.logic_flags(v); c.C = true; c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9401) {                             // neg
        uint8_t a = c.r[rd5()], v = uint8_t(0u - a);
        c.logic_flags(v); c.C = v != 0; c.V = v == 0x80;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9403) {                             // inc
        uint8_t v = uint8_t(c.r[rd5()] + 1);
        c.V = v == 0x80; c.N = (v & 0x80) != 0; c.Z = v == 0;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x940A) {                             // dec
        uint8_t v = uint8_t(c.r[rd5()] - 1);
        c.V = v == 0x7F; c.N = (v & 0x80) != 0; c.Z = v == 0;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9405) {                             // asr
        uint8_t a = c.r[rd5()], v = uint8_t((a >> 1) | (a & 0x80));
        c.C = (a & 1) != 0; c.N = (v & 0x80) != 0; c.Z = v == 0; c.V = c.N != c.C;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9406) {                             // lsr
        uint8_t a = c.r[rd5()], v = uint8_t(a >> 1);
        c.C = (a & 1) != 0; c.N = false; c.Z = v == 0; c.V = c.C;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9407) {                             // ror
        uint8_t a = c.r[rd5()], v = uint8_t((a >> 1) | (c.C ? 0x80 : 0));
        c.C = (a & 1) != 0; c.N = (v & 0x80) != 0; c.Z = v == 0; c.V = c.N != c.C;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xD000) == 0x8000) {                             // ld/st with q
        unsigned q = (op & 7) | ((op >> 7) & 0x18) | ((op >> 8) & 0x20);
        unsigned base = (op & 8) ? 28u : 30u;                  // Y or Z
        uint16_t addr = uint16_t((c.r[base] | (c.r[base + 1] << 8)) + q);
        if (op & 0x0200) c.store(addr, c.r[rd5()]);
        else c.r[rd5()] = c.load(addr);
        return true;
    }
    if ((op & 0xFC00) == 0x9000) {                             // ld/st, X/Y/Z+-
        unsigned kind = op & 0x0F;
        unsigned base = (kind == 0x0C || kind == 0x0D || kind == 0x0E) ? 26u
                      : (kind == 0x08 || kind == 0x09 || kind == 0x0A) ? 28u : 30u;
        uint16_t p = uint16_t(c.r[base] | (c.r[base + 1] << 8));
        bool post = kind == 0x01 || kind == 0x09 || kind == 0x0D;
        bool pre  = kind == 0x02 || kind == 0x0A || kind == 0x0E;
        if (pre) --p;
        if (op & 0x0200) c.store(p, c.r[rd5()]);
        else c.r[rd5()] = c.load(p);
        uint16_t after = post ? uint16_t(p + 1) : p;
        c.r[base] = uint8_t(after); c.r[base + 1] = uint8_t(after >> 8);
        return true;
    }
    if ((op & 0xF000) == 0xB000) {                             // in / out
        unsigned a = (op & 0x0F) | ((op >> 5) & 0x30);
        // Only the three ports the prologue and epilogue touch are modelled:
        // the stack pointer's halves and the status register.
        auto sreg = [&]() -> uint8_t {
            return uint8_t((c.C ? 1 : 0) | (c.Z ? 2 : 0) | (c.N ? 4 : 0) |
                           (c.V ? 8 : 0) | (c.S() ? 0x10 : 0) | (c.T ? 0x40 : 0));
        };
        if (op & 0x0800) {                                     // out
            uint8_t v = c.r[rd5()];
            if (a == 0x3D) c.sp = uint16_t((c.sp & 0xFF00) | v);
            else if (a == 0x3E) c.sp = uint16_t((c.sp & 0x00FF) | (v << 8));
            else if (a == 0x3F) {
                c.C = v & 1; c.Z = v & 2; c.N = v & 4; c.V = v & 8; c.T = v & 0x40;
            }
        } else {                                               // in
            uint8_t v = 0;
            if (a == 0x3D) v = uint8_t(c.sp & 0xFF);
            else if (a == 0x3E) v = uint8_t(c.sp >> 8);
            else if (a == 0x3F) v = sreg();
            c.r[rd5()] = v;
        }
        return true;
    }
    if (op == 0x94F8 || op == 0x9478) return true;             // cli / sei
    if (op == 0x0000) return true;                             // nop

    char buf[64];
    std::snprintf(buf, sizeof buf, "unknown opcode 0x%04X at word %u",
                  unsigned(op), unsigned(here));
    c.error = buf;
    c.stopped = true;
    return false;
}

// Runs an image from word 0 with a sentinel return address on the stack, so the
// first `ret` that unwinds past the entry point ends the run.
bool run(Cpu& c, const std::vector<uint8_t>& image, std::string& error) {
    c.image = &image;
    c.push(0xFF);
    c.push(0xFF);
    for (long steps = 0; steps < 2000000; ++steps) {
        if (!step(c)) {
            error = c.error;
            return c.error.empty();
        }
    }
    error = "the program did not stop";
    return false;
}

} // namespace sim

namespace {

TypePtr long_type() { return make_type(TypeKind::Long); }
TypePtr ulong_type() { return make_type(TypeKind::ULong); }

// Assembles `text` -- with a trailing `ret` and the 32-bit runtime appended --
// and runs it. Returns false with an explanation if anything refuses.
bool run_snippet(const std::string& text, sim::Cpu& cpu, std::string& why) {
    std::string math32;
    if (!read_runtime("runtime/math32.S", math32)) {
        why = "skip";
        return false;
    }
    AssembleResult r = assemble(text + "\nret\n" + math32 + "\n");
    if (!r.ok) { why = "assembler: " + r.error; return false; }
    return sim::run(cpu, r.code, why);
}

uint32_t wide_result(const sim::Cpu& c) {
    return uint32_t(c.r[22]) | (uint32_t(c.r[23]) << 8) |
           (uint32_t(c.r[24]) << 16) | (uint32_t(c.r[25]) << 24);
}

// Evaluates one 32-bit expression on the interpreter and compares the value in
// r22..r25 against `expected`.
bool eval32(const Expr& e, uint32_t& value, std::string& why) {
    CodeGen cg;
    cg.gen_expr(e);
    if (!cg.error.empty()) { why = "codegen: " + cg.error; return false; }
    sim::Cpu cpu;
    cpu.r[1] = 0;
    if (!run_snippet(cg.out, cpu, why)) return false;
    if (cpu.r[1] != 0) { why = "r1 was left non-zero"; return false; }
    value = wide_result(cpu);
    return true;
}

// Runs one long-valued binary operation and reports the answer.
bool eval_binop32(const char* op, long a, long b, TypePtr t, uint32_t& value,
                  std::string& why) {
    return eval32(*binary(op, literal(a, t), literal(b, t), t), value, why);
}

} // namespace

// ---- literals, loads and stores --------------------------------------------

TEST(codegen_long_literal_fills_all_four_registers) {
    CodeGen cg;
    std::string text = gen(*literal(0x12345678L, long_type()), cg);
    CHECK(cg.error.empty());
    CHECK(text == "ldi r22, 120\nldi r23, 86\nldi r24, 52\nldi r25, 18\n");
}

TEST(codegen_long_local_uses_four_frame_bytes) {
    CodeGen cg;
    cg.set_local_offset("t", 4);
    std::string text = gen(*identifier("t", long_type()), cg);
    CHECK(cg.error.empty());
    CHECK(text == "ldd r22, Y+4\nldd r23, Y+5\nldd r24, Y+6\nldd r25, Y+7\n");
}

TEST(codegen_long_global_uses_four_addresses) {
    CodeGen cg;
    cg.set_global_address("ms", 0x0200);
    std::string text = gen(*identifier("ms", long_type()), cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "lds r22, 512"));
    CHECK(contains(text, "lds r25, 515"));
}

TEST(codegen_long_local_beyond_the_frame_pointer_is_rejected) {
    CodeGen cg;
    cg.set_local_offset("t", 61);                 // 61..64 -- one byte too far
    gen(*identifier("t", long_type()), cg);
    CHECK(!cg.error.empty());
    CHECK(contains(cg.error, "too far from the frame pointer"));
}

TEST(codegen_long_assignment_stores_four_bytes) {
    CodeGen cg;
    cg.set_global_address("ms", 0x0200);
    auto a = assign("=", identifier("ms", long_type()), literal(7, long_type()));
    a->type = long_type();
    std::string text = gen(*a, cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "sts 512, r22"));
    CHECK(contains(text, "sts 515, r25"));
}

TEST(codegen_long_round_trips_through_a_global) {
    CodeGen cg;
    cg.set_global_address("ms", 0x0200);
    auto a = assign("=", identifier("ms", long_type()),
                    literal(0x0BADF00DL, long_type()));
    a->type = long_type();
    cg.gen_expr(*a);
    cg.gen_expr(*identifier("ms", long_type()));
    CHECK(cg.error.empty());
    sim::Cpu cpu;
    std::string why;
    if (!run_snippet(cg.out, cpu, why)) {
        std::printf("    %s\n", why.c_str());
        CHECK(why == "skip");
        return;
    }
    CHECK_EQ(wide_result(cpu), uint32_t(0x0BADF00D));
}

// ---- arithmetic, run rather than inspected ---------------------------------

TEST(codegen_long_add_and_subtract_carry_across_all_four_bytes) {
    struct { const char* op; long a; long b; uint32_t want; } cases[] = {
        {"+", 0x0000FFFFL, 1L,          0x00010000u},
        {"+", 0x00FFFFFFL, 1L,          0x01000000u},
        {"+", 1000000L,    2000000L,    3000000u},
        {"-", 0x00010000L, 1L,          0x0000FFFFu},
        {"-", 0L,          1L,          0xFFFFFFFFu},
        {"-", 3000000L,    1000000L,    2000000u},
    };
    for (auto& c : cases) {
        uint32_t got = 0;
        std::string why;
        if (!eval_binop32(c.op, c.a, c.b, long_type(), got, why)) {
            std::printf("    %s\n", why.c_str());
            CHECK(why == "skip");
            return;
        }
        if (got != c.want)
            std::printf("    %ld %s %ld gave %u\n", c.a, c.op, c.b, unsigned(got));
        CHECK_EQ(got, c.want);
    }
}

TEST(codegen_long_bitwise_operators_work_on_every_byte) {
    struct { const char* op; long a; long b; uint32_t want; } cases[] = {
        {"&", 0x12345678L, 0x0F0F0F0FL, 0x02040608u},
        {"|", 0x12340000L, 0x00005678L, 0x12345678u},
        {"^", 0x12345678L, 0xFFFFFFFL,  0x12345678u ^ 0x0FFFFFFFu},
    };
    for (auto& c : cases) {
        uint32_t got = 0;
        std::string why;
        if (!eval_binop32(c.op, c.a, c.b, ulong_type(), got, why)) {
            std::printf("    %s\n", why.c_str());
            CHECK(why == "skip");
            return;
        }
        CHECK_EQ(got, c.want);
    }
}

TEST(codegen_long_negate_and_complement) {
    uint32_t got = 0;
    std::string why;
    if (!eval32(*unary("-", literal(1000000L, long_type()), false, long_type()),
                got, why)) {
        std::printf("    %s\n", why.c_str());
        CHECK(why == "skip");
        return;
    }
    CHECK_EQ(got, uint32_t(-1000000));

    CHECK(eval32(*unary("-", literal(0L, long_type()), false, long_type()),
                 got, why));
    CHECK_EQ(got, uint32_t(0));

    CHECK(eval32(*unary("~", literal(0x0F0F0F0FL, ulong_type()), false,
                        ulong_type()), got, why));
    CHECK_EQ(got, uint32_t(0xF0F0F0F0));
}

TEST(codegen_long_shifts_move_bits_across_byte_boundaries) {
    struct { const char* op; long a; long n; TypePtr(*t)(); uint32_t want; } cases[] = {
        {"<<", 1L,           20L, long_type,  0x00100000u},
        {"<<", 0x0000FFFFL,  8L,  long_type,  0x00FFFF00u},
        {">>", 0x12345678L,  16L, ulong_type, 0x00001234u},
        {">>", -256L,        4L,  long_type,  uint32_t(-16)},
        {">>", -1L,          8L,  long_type,  0xFFFFFFFFu},
    };
    for (auto& c : cases) {
        TypePtr t = c.t();
        uint32_t got = 0;
        std::string why;
        if (!eval32(*binary(c.op, literal(c.a, t), literal(c.n, t), t), got, why)) {
            std::printf("    %s\n", why.c_str());
            CHECK(why == "skip");
            return;
        }
        if (got != c.want)
            std::printf("    %ld %s %ld gave 0x%08X\n", c.a, c.op, c.n, unsigned(got));
        CHECK_EQ(got, c.want);
    }
}

TEST(codegen_long_shift_right_of_an_unsigned_value_does_not_sign_extend) {
    uint32_t got = 0;
    std::string why;
    if (!eval32(*binary(">>", literal(-1L, ulong_type()), literal(8L, ulong_type()),
                        ulong_type()), got, why)) {
        std::printf("    %s\n", why.c_str());
        CHECK(why == "skip");
        return;
    }
    CHECK_EQ(got, uint32_t(0x00FFFFFF));
}

// ---- multiply, divide, modulo ----------------------------------------------

TEST(codegen_long_multiply_calls_the_helper) {
    CodeGen cg;
    std::string text = gen(*binary("*", literal(3, long_type()),
                                   literal(4, long_type()), long_type()), cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "call __ardio_mul32"));
}

TEST(runtime_math32_multiply_computes_the_low_32_bits) {
    struct { long a; long b; uint32_t want; } cases[] = {
        {0, 12345, 0u},
        {1, 0x12345678L, 0x12345678u},
        {1000, 1000, 1000000u},
        {65536L, 65536L, 0u},                       // wraps to zero, as C says
        {0x00010001L, 0x00010001L, 0x00020001u},
        {123456L, 65537L, uint32_t(123456u * 65537u)},
        {-3, 7, uint32_t(-21)},
    };
    for (auto& c : cases) {
        uint32_t got = 0;
        std::string why;
        if (!eval_binop32("*", c.a, c.b, long_type(), got, why)) {
            std::printf("    %s\n", why.c_str());
            CHECK(why == "skip");
            return;
        }
        if (got != c.want)
            std::printf("    %ld * %ld gave %u\n", c.a, c.b, unsigned(got));
        CHECK_EQ(got, c.want);
    }
}

TEST(runtime_math32_unsigned_divide_and_modulo) {
    struct { const char* op; unsigned long a; unsigned long b; uint32_t want; } cases[] = {
        {"/", 1000000UL, 1000UL, 1000u},
        {"/", 0xFFFFFFFFUL, 0xFFFFUL, 0x10001u},
        {"/", 7UL, 8UL, 0u},
        {"/", 4000000000UL, 7UL, uint32_t(4000000000UL / 7UL)},
        {"%", 1000000UL, 7UL, uint32_t(1000000UL % 7UL)},
        {"%", 0xFFFFFFFFUL, 10UL, 5u},
        {"%", 8UL, 8UL, 0u},
    };
    for (auto& c : cases) {
        uint32_t got = 0;
        std::string why;
        if (!eval_binop32(c.op, long(c.a), long(c.b), ulong_type(), got, why)) {
            std::printf("    %s\n", why.c_str());
            CHECK(why == "skip");
            return;
        }
        if (got != c.want)
            std::printf("    %lu %s %lu gave %u\n", c.a, c.op, c.b, unsigned(got));
        CHECK_EQ(got, c.want);
    }
}

TEST(runtime_math32_signed_divide_truncates_toward_zero) {
    struct { const char* op; long a; long b; int32_t want; } cases[] = {
        {"/", 1000000L, 7L, 1000000 / 7},
        {"/", -1000000L, 7L, -1000000 / 7},
        {"/", 1000000L, -7L, 1000000 / -7},
        {"/", -1000000L, -7L, -1000000 / -7},
        {"%", -1000000L, 7L, -1000000 % 7},
        {"%", 1000000L, -7L, 1000000 % -7},
        {"%", -7L, 1000000L, -7 % 1000000},
    };
    for (auto& c : cases) {
        uint32_t got = 0;
        std::string why;
        if (!eval_binop32(c.op, c.a, c.b, long_type(), got, why)) {
            std::printf("    %s\n", why.c_str());
            CHECK(why == "skip");
            return;
        }
        if (int32_t(got) != c.want)
            std::printf("    %ld %s %ld gave %d\n", c.a, c.op, c.b, int32_t(got));
        CHECK_EQ(int32_t(got), c.want);
    }
}

TEST(codegen_long_division_leaves_r1_zero) {
    // MUL destroys the zero register, so anything that multiplies has to put it
    // back. eval32() checks r1 after every run; this test says so out loud.
    uint32_t got = 0;
    std::string why;
    if (!eval32(*binary("*", binary("*", literal(1234L, long_type()),
                                    literal(5678L, long_type()), long_type()),
                        literal(3L, long_type()), long_type()), got, why)) {
        std::printf("    %s\n", why.c_str());
        CHECK(why == "skip");
        return;
    }
    CHECK_EQ(got, uint32_t(1234u * 5678u * 3u));
}

// ---- widening and narrowing -------------------------------------------------

TEST(codegen_widening_an_int_to_a_long_sign_extends) {
    CodeGen cg;
    auto c = std::make_unique<Expr>();
    c->kind = ExprKind::Cast;
    c->lhs = literal(-2, int_type());
    c->type = long_type();
    std::string text = gen(*c, cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "movw r22, r24"));
    CHECK(contains(text, "sbrc r23, 7"));

    uint32_t got = 0;
    std::string why;
    if (!eval32(*c, got, why)) { std::printf("    %s\n", why.c_str());
                                 CHECK(why == "skip"); return; }
    CHECK_EQ(got, uint32_t(-2));
}

TEST(codegen_widening_an_unsigned_int_to_a_long_zero_extends) {
    auto c = std::make_unique<Expr>();
    c->kind = ExprKind::Cast;
    c->lhs = literal(0xFFFF, uint_type());
    c->type = ulong_type();
    uint32_t got = 0;
    std::string why;
    if (!eval32(*c, got, why)) { std::printf("    %s\n", why.c_str());
                                 CHECK(why == "skip"); return; }
    CHECK_EQ(got, uint32_t(0x0000FFFF));
}

TEST(codegen_widening_a_char_to_a_long_sign_extends) {
    auto c = std::make_unique<Expr>();
    c->kind = ExprKind::Cast;
    c->lhs = literal(-5, char_type());
    c->type = long_type();
    uint32_t got = 0;
    std::string why;
    if (!eval32(*c, got, why)) { std::printf("    %s\n", why.c_str());
                                 CHECK(why == "skip"); return; }
    CHECK_EQ(got, uint32_t(-5));
}

TEST(codegen_narrowing_a_long_to_an_int_keeps_the_low_half) {
    CodeGen cg;
    auto c = std::make_unique<Expr>();
    c->kind = ExprKind::Cast;
    c->lhs = literal(0x12345678L, long_type());
    c->type = int_type();
    std::string text = gen(*c, cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "movw r24, r22"));

    sim::Cpu cpu;
    std::string why;
    if (!run_snippet(cg.out, cpu, why)) { std::printf("    %s\n", why.c_str());
                                          CHECK(why == "skip"); return; }
    CHECK_EQ(unsigned(cpu.r[24] | (cpu.r[25] << 8)), 0x5678u);
}

TEST(codegen_narrowing_a_long_to_a_char_re_extends) {
    auto c = std::make_unique<Expr>();
    c->kind = ExprKind::Cast;
    c->lhs = literal(0x12345680L, long_type());
    c->type = char_type();
    CodeGen cg;
    std::string text = gen(*c, cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "movw r24, r22"));
    CHECK(contains(text, "sbrc r24, 7"));

    sim::Cpu cpu;
    std::string why;
    if (!run_snippet(cg.out, cpu, why)) { std::printf("    %s\n", why.c_str());
                                          CHECK(why == "skip"); return; }
    CHECK_EQ(unsigned(cpu.r[24] | (cpu.r[25] << 8)), 0xFF80u);   // sign-extended
}

TEST(codegen_mixed_width_arithmetic_promotes_to_32_bits) {
    // 100000L * 3 with a plain int on the right: the int is widened before the
    // multiply, so the answer is not computed at 16 bits and truncated.
    uint32_t got = 0;
    std::string why;
    if (!eval32(*binary("*", literal(100000L, long_type()), literal(3, int_type()),
                        long_type()), got, why)) {
        std::printf("    %s\n", why.c_str());
        CHECK(why == "skip");
        return;
    }
    CHECK_EQ(got, uint32_t(300000));
}

// ---- comparisons ------------------------------------------------------------

TEST(codegen_long_comparisons_compare_all_four_bytes) {
    struct { const char* op; long a; long b; bool want; } cases[] = {
        {"==", 0x00010000L, 0x00010000L, true},
        {"==", 0x00010000L, 0x00000000L, false},
        {"!=", 0x00010000L, 0x00000000L, true},
        {"<",  0x0000FFFFL, 0x00010000L, true},
        {"<",  0x00010000L, 0x0000FFFFL, false},
        {">",  0x00010000L, 0x0000FFFFL, true},
        {"<=", 0x00010000L, 0x00010000L, true},
        {">=", 0x00010000L, 0x00010001L, false},
        {"<",  -1L,         1L,          true},     // signed
        {">",  -1L,         -2L,         true},
        {"<=", -5L,         -5L,         true},
    };
    for (auto& c : cases) {
        CodeGen cg;
        cg.gen_expr(*binary(c.op, literal(c.a, long_type()),
                            literal(c.b, long_type()), int_type()));
        CHECK(cg.error.empty());
        sim::Cpu cpu;
        std::string why;
        if (!run_snippet(cg.out, cpu, why)) {
            std::printf("    %s\n", why.c_str());
            CHECK(why == "skip");
            return;
        }
        unsigned got = unsigned(cpu.r[24] | (cpu.r[25] << 8));
        if (got != unsigned(c.want ? 1 : 0))
            std::printf("    %ld %s %ld gave %u\n", c.a, c.op, c.b, got);
        CHECK_EQ(got, unsigned(c.want ? 1 : 0));
    }
}

TEST(codegen_unsigned_long_comparison_does_not_use_a_signed_branch) {
    CodeGen cg;
    std::string text = gen(*binary("<", literal(1L, ulong_type()),
                                   literal(0xFFFFFFFFL, ulong_type()), int_type()), cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "brlo"));
    CHECK(!contains(text, "brlt"));

    sim::Cpu cpu;
    std::string why;
    if (!run_snippet(cg.out, cpu, why)) { std::printf("    %s\n", why.c_str());
                                          CHECK(why == "skip"); return; }
    CHECK_EQ(unsigned(cpu.r[24] | (cpu.r[25] << 8)), 1u);
}

TEST(codegen_long_truth_test_looks_at_every_byte) {
    // A value whose low 16 bits are zero is still true.
    CodeGen cg;
    cg.gen_expr(*unary("!", literal(0x00010000L, long_type()), false, int_type()));
    CHECK(cg.error.empty());
    sim::Cpu cpu;
    std::string why;
    if (!run_snippet(cg.out, cpu, why)) { std::printf("    %s\n", why.c_str());
                                          CHECK(why == "skip"); return; }
    CHECK_EQ(unsigned(cpu.r[24] | (cpu.r[25] << 8)), 0u);       // !nonzero == 0
}

// ---- increment --------------------------------------------------------------

TEST(codegen_long_increment_carries_into_the_high_bytes) {
    CodeGen cg;
    cg.set_global_address("ms", 0x0200);
    auto a = assign("=", identifier("ms", long_type()),
                    literal(0x0000FFFFL, long_type()));
    a->type = long_type();
    cg.gen_expr(*a);
    cg.gen_expr(*unary("++", identifier("ms", long_type()), false, long_type()));
    CHECK(cg.error.empty());
    CHECK(contains(cg.out, "subi r22, -1"));
    CHECK(contains(cg.out, "sbci r25, -1"));
    sim::Cpu cpu;
    std::string why;
    if (!run_snippet(cg.out, cpu, why)) { std::printf("    %s\n", why.c_str());
                                          CHECK(why == "skip"); return; }
    CHECK_EQ(wide_result(cpu), uint32_t(0x00010000));
}

TEST(codegen_long_decrement_borrows_from_the_high_bytes) {
    CodeGen cg;
    cg.set_global_address("ms", 0x0200);
    auto a = assign("=", identifier("ms", long_type()),
                    literal(0x00010000L, long_type()));
    a->type = long_type();
    cg.gen_expr(*a);
    cg.gen_expr(*unary("--", identifier("ms", long_type()), true, long_type()));
    CHECK(cg.error.empty());
    sim::Cpu cpu;
    std::string why;
    if (!run_snippet(cg.out, cpu, why)) { std::printf("    %s\n", why.c_str());
                                          CHECK(why == "skip"); return; }
    CHECK_EQ(wide_result(cpu), uint32_t(0x00010000));           // postfix value
    uint32_t stored = uint32_t(cpu.mem[0x200]) | (uint32_t(cpu.mem[0x201]) << 8) |
                      (uint32_t(cpu.mem[0x202]) << 16) | (uint32_t(cpu.mem[0x203]) << 24);
    CHECK_EQ(stored, uint32_t(0x0000FFFF));
}

// ---- pointers and members ---------------------------------------------------

TEST(codegen_long_through_a_pointer_moves_four_bytes) {
    CodeGen cg;
    cg.set_global_address("p", 0x0300);
    auto deref = unary("*", identifier("p", make_pointer(long_type())), false,
                       long_type());
    std::string text = gen(*deref, cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "ld r22, Z"));
    CHECK(contains(text, "ldd r23, Z+1"));
    CHECK(contains(text, "ldd r24, Z+2"));
    CHECK(contains(text, "ldd r25, Z+3"));
}

TEST(codegen_long_array_element_is_read_whole) {
    CodeGen cg;
    cg.set_global_address("times", 0x0400);
    TypePtr array = make_array(long_type(), 4);
    std::string text = gen(*index(identifier("times", array), literal(2),
                                  long_type()), cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "lsl r24"));             // index * 4
    CHECK(contains(text, "ldd r25, Z+3"));
}

TEST(codegen_long_array_element_round_trips) {
    CodeGen cg;
    cg.set_global_address("times", 0x0400);
    TypePtr array = make_array(long_type(), 4);
    auto store = assign("=", index(identifier("times", array), literal(2),
                                   long_type()),
                        literal(0x11223344L, long_type()));
    store->type = long_type();
    cg.gen_expr(*store);
    cg.gen_expr(*index(identifier("times", array), literal(2), long_type()));
    CHECK(cg.error.empty());
    sim::Cpu cpu;
    std::string why;
    if (!run_snippet(cg.out, cpu, why)) { std::printf("    %s\n", why.c_str());
                                          CHECK(why == "skip"); return; }
    CHECK_EQ(wide_result(cpu), uint32_t(0x11223344));
}

// ---- calls, parameters and returns -----------------------------------------

TEST(codegen_long_argument_occupies_r22_to_r25) {
    CodeGen cg;
    auto call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->name = "delay";
    call->type = make_type(TypeKind::Void);
    call->args.push_back(literal(1000L, long_type()));
    std::string text = gen(*call, cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "push r22"));
    CHECK(contains(text, "pop r25"));
    CHECK(contains(text, "pop r22"));
    CHECK(contains(text, "call delay"));
}

TEST(codegen_long_argument_pushes_the_next_argument_down) {
    // f(long, int): the long takes r22..r25, so the int lands in r20:r21.
    CodeGen cg;
    auto call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->name = "f";
    call->type = int_type();
    call->args.push_back(literal(1L, long_type()));
    call->args.push_back(literal(2, int_type()));
    std::string text = gen(*call, cg);
    CHECK(cg.error.empty());
    CHECK(contains(text, "pop r21"));
    CHECK(contains(text, "pop r20"));
}

TEST(codegen_long_parameter_is_spilled_whole) {
    CodeGen cg;
    Function f;
    f.name = "use_millis";
    f.return_type = long_type();
    f.params.push_back({"ms", long_type()});
    auto body = std::make_unique<Stmt>();
    body->kind = StmtKind::Block;
    auto ret = std::make_unique<Stmt>();
    ret->kind = StmtKind::Return;
    ret->expr = identifier("ms", long_type());
    body->body.push_back(std::move(ret));
    f.body = std::move(body);

    cg.gen_function(f);
    CHECK(cg.error.empty());
    CHECK(contains(cg.out, "std  Y+1, r22"));
    CHECK(contains(cg.out, "std  Y+2, r23"));
    CHECK(contains(cg.out, "std  Y+3, r24"));
    CHECK(contains(cg.out, "std  Y+4, r25"));
    CHECK(contains(cg.out, "ldd r22, Y+1"));
    AssembleResult r = assemble(cg.out);
    if (!r.ok) std::printf("    assembler said: %s\n", r.error.c_str());
    CHECK(r.ok);
}

TEST(codegen_long_function_round_trips_its_argument) {
    CodeGen cg;
    Function f;
    f.name = "twice";
    f.return_type = long_type();
    f.params.push_back({"ms", long_type()});
    auto body = std::make_unique<Stmt>();
    body->kind = StmtKind::Block;
    auto ret = std::make_unique<Stmt>();
    ret->kind = StmtKind::Return;
    ret->expr = binary("+", identifier("ms", long_type()),
                       identifier("ms", long_type()), long_type());
    body->body.push_back(std::move(ret));
    f.body = std::move(body);
    cg.gen_function(f);
    CHECK(cg.error.empty());

    // Call it with 0x00201234 and check the doubled value comes back.
    std::string program =
        "ldi r22, 0x34\nldi r23, 0x12\nldi r24, 0x20\nldi r25, 0\n"
        "call twice\nret\n" + cg.out;
    AssembleResult r = assemble(program);
    if (!r.ok) std::printf("    assembler said: %s\n", r.error.c_str());
    CHECK(r.ok);
    if (!r.ok) return;
    sim::Cpu cpu;
    std::string why;
    if (!sim::run(cpu, r.code, why)) { std::printf("    %s\n", why.c_str());
                                       CHECK(why.empty()); return; }
    CHECK_EQ(wide_result(cpu), uint32_t(0x00201234 * 2));
}

TEST(codegen_int_returned_from_a_long_expression_is_narrowed_not_truncated) {
    CodeGen cg;
    Function f;
    f.name = "low_half";
    f.return_type = int_type();
    auto body = std::make_unique<Stmt>();
    body->kind = StmtKind::Block;
    auto ret = std::make_unique<Stmt>();
    ret->kind = StmtKind::Return;
    ret->expr = literal(0x12345678L, long_type());
    body->body.push_back(std::move(ret));
    f.body = std::move(body);
    cg.gen_function(f);
    CHECK(cg.error.empty());
    CHECK(contains(cg.out, "movw r24, r22"));
}

TEST(codegen_long_local_declaration_stores_four_bytes) {
    CodeGen cg;
    Function f;
    f.name = "count";
    f.return_type = make_type(TypeKind::Void);
    auto body = std::make_unique<Stmt>();
    body->kind = StmtKind::Block;
    auto decl = std::make_unique<Stmt>();
    decl->kind = StmtKind::VarDecl;
    decl->var_name = "t";
    decl->var_type = long_type();
    decl->var_init = literal(0x01020304L, long_type());
    body->body.push_back(std::move(decl));
    f.body = std::move(body);
    cg.gen_function(f);
    CHECK(cg.error.empty());
    CHECK(contains(cg.out, "std Y+1, r22"));
    CHECK(contains(cg.out, "std Y+4, r25"));
    AssembleResult r = assemble(cg.out);
    if (!r.ok) std::printf("    assembler said: %s\n", r.error.c_str());
    CHECK(r.ok);
}

TEST(codegen_int_local_initialised_from_a_long_is_narrowed) {
    CodeGen cg;
    Function f;
    f.name = "narrow";
    f.return_type = make_type(TypeKind::Void);
    auto body = std::make_unique<Stmt>();
    body->kind = StmtKind::Block;
    auto decl = std::make_unique<Stmt>();
    decl->kind = StmtKind::VarDecl;
    decl->var_name = "n";
    decl->var_type = int_type();
    decl->var_init = literal(0x12345678L, long_type());
    body->body.push_back(std::move(decl));
    f.body = std::move(body);
    cg.gen_function(f);
    CHECK(cg.error.empty());
    CHECK(contains(cg.out, "movw r24, r22"));     // convert, do not truncate
}

TEST(codegen_long_condition_in_a_while_tests_four_bytes) {
    CodeGen cg;
    Function f;
    f.name = "spin";
    f.return_type = make_type(TypeKind::Void);
    auto body = std::make_unique<Stmt>();
    body->kind = StmtKind::Block;
    auto loop = std::make_unique<Stmt>();
    loop->kind = StmtKind::While;
    loop->expr = identifier("ms", long_type());
    auto inner = std::make_unique<Stmt>();
    inner->kind = StmtKind::Empty;
    loop->then_branch = std::move(inner);
    body->body.push_back(std::move(loop));
    f.body = std::move(body);
    cg.gen_function(f);
    CHECK(cg.error.empty());
    CHECK(contains(cg.out, "cp   r22, r1"));
    CHECK(contains(cg.out, "cpc  r25, r1"));
}

// ---- the runtime file itself -----------------------------------------------

TEST(runtime_math32_assembles_on_its_own) {
    std::string math;
    if (!read_runtime("runtime/math32.S", math)) {
        std::printf("  skip runtime/math32.S not found relative to the cwd\n");
        return;
    }
    AssembleResult r = assemble(math);
    if (!r.ok) std::printf("    assembler said: %s\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(!r.code.empty());
    CHECK_EQ(r.code.size() % 2, size_t(0));
}

TEST(runtime_math32_defines_every_helper) {
    std::string math;
    if (!read_runtime("runtime/math32.S", math)) {
        std::printf("  skip runtime/math32.S not found relative to the cwd\n");
        return;
    }
    for (const char* label : {"__ardio_mul32:", "__ardio_udivmod32:",
                              "__ardio_divmod32:"}) {
        if (math.find(label) == std::string::npos)
            std::printf("    missing entry point %s\n", label);
        CHECK(math.find(label) != std::string::npos);
    }
}

// A wide sweep rather than a handful of chosen cases: the same deterministic
// pseudo-random operands are pushed through the generator and through the
// host's own 32-bit arithmetic, and the two answers have to agree. This is
// what catches a carry that is right for small numbers and wrong for large.
TEST(runtime_math32_agrees_with_the_hosts_arithmetic_over_a_sweep) {
    uint32_t seed = 0x2545F491u;
    auto next = [&seed]() {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return seed;
    };
    for (int i = 0; i < 40; ++i) {
        uint32_t ua = next(), ub = next();
        if (ub == 0) ub = 1;
        struct { const char* op; bool is_signed; uint32_t want; } trials[] = {
            {"*", false, uint32_t(ua * ub)},
            {"/", false, uint32_t(ua / ub)},
            {"%", false, uint32_t(ua % ub)},
            {"*", true,  uint32_t(int32_t(ua) * int32_t(ub))},
            {"/", true,  uint32_t(int32_t(ua) / int32_t(ub))},
            {"%", true,  uint32_t(int32_t(ua) % int32_t(ub))},
        };
        for (auto& t : trials) {
            // INT32_MIN / -1 overflows in C too; skip the one undefined case.
            if (t.is_signed && int32_t(ua) == INT32_MIN && int32_t(ub) == -1) continue;
            TypePtr type = t.is_signed ? long_type() : ulong_type();
            uint32_t got = 0;
            std::string why;
            if (!eval_binop32(t.op, long(int32_t(ua)), long(int32_t(ub)), type,
                              got, why)) {
                std::printf("    %s\n", why.c_str());
                CHECK(why == "skip");
                return;
            }
            if (got != t.want)
                std::printf("    0x%08X %s%s 0x%08X gave 0x%08X, wanted 0x%08X\n",
                            unsigned(ua), t.is_signed ? "s" : "u", t.op,
                            unsigned(ub), unsigned(got), unsigned(t.want));
            CHECK_EQ(got, t.want);
        }
    }
}
