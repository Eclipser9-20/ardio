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
