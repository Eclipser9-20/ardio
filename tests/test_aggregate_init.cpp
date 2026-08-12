// Aggregate initialisers: braced lists and the string-literal form, at global
// and at block scope.
//
// These tests prove values, not just that something compiled. Globals are
// initialised by a run of `ldi`/`sts` in the startup preamble, and locals by
// stores into the frame, so both are checked by executing the generated
// assembly on a tiny interpreter for exactly the instruction subset the
// generator emits, then reading the resulting memory back. Where a test only
// needs to know the output is well-formed it runs the real assembler over it.

#include "harness.h"
#include "ardio/avr/assembler.h"
#include "ardio/avr/compiler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using ardio::CompileResult;

CompileResult build(const std::string& source) {
    return ardio::compile_avr(source);
}

CompileResult sketch(const std::string& fragment) {
    return build(fragment + "\nvoid setup() {}\nvoid loop() {}\n");
}

// --------------------------------------------------------- tiny AVR core ---
//
// Enough of the instruction set to run what the generator produces for an
// initialiser: immediate loads, the frame/pointer arithmetic behind a
// subscript, and the loads and stores at either end. Anything else is
// ignored, which is safe here because a test asserts on memory the
// initialiser wrote rather than on the machine reaching a particular state.

struct Machine {
    uint8_t reg[32] = {};
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x0900, 0);
    int sp = 0x08FF;

    int frame = -1;                 // Y right after the prologue moved it down

    int pair(int r) const { return reg[r] | (reg[r + 1] << 8); }
    void set_pair(int r, int v) {
        reg[r] = static_cast<uint8_t>(v & 0xFF);
        reg[r + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
    uint8_t& at(int addr) { return mem[static_cast<size_t>(addr) % mem.size()]; }
};

std::vector<std::string> operands(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == ',') { parts.push_back(current); current.clear(); }
        else current.push_back(c);
    }
    parts.push_back(current);
    for (std::string& p : parts) {
        size_t a = p.find_first_not_of(" \t");
        if (a == std::string::npos) { p.clear(); continue; }
        p = p.substr(a, p.find_last_not_of(" \t") - a + 1);
    }
    return parts;
}

int reg_number(const std::string& op) {
    if (op.size() < 2 || op[0] != 'r') return -1;
    return std::stoi(op.substr(1));
}

// A displacement operand: "Y+3", "Z+1", or a bare "Z".
int displacement(const std::string& op) {
    size_t plus = op.find('+');
    return plus == std::string::npos ? 0 : std::stoi(op.substr(plus + 1));
}

long literal(const std::string& op) {
    return std::stol(op, nullptr, op.compare(0, 2, "0x") == 0 ? 16 : 10);
}

// Runs the lines of `text` between `from` and the next line equal to `stop`
// (or to the end when `stop` is empty).
void run(Machine& m, const std::string& text, const std::string& begin_after,
         const std::string& stop_at) {
    bool running = begin_after.empty();
    size_t i = 0;
    while (i <= text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(i, end - i);
        i = end + 1;
        if (i > text.size() + 1) break;

        size_t comment = line.find(';');
        if (comment != std::string::npos) line = line.substr(0, comment);
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        line = line.substr(a, line.find_last_not_of(" \t") - a + 1);
        if (line.empty()) continue;

        if (!running) { if (line == begin_after) running = true; continue; }
        if (!stop_at.empty() && line == stop_at) return;

        size_t sp_pos = line.find_first_of(" \t");
        std::string op = line.substr(0, sp_pos);
        std::vector<std::string> a_ = operands(sp_pos == std::string::npos ? ""
                                                                          : line.substr(sp_pos));

        if (op == "ldi") m.reg[reg_number(a_[0])] = static_cast<uint8_t>(literal(a_[1]));
        else if (op == "clr") m.reg[reg_number(a_[0])] = 0;
        else if (op == "movw") m.set_pair(reg_number(a_[0]), m.pair(reg_number(a_[1])));
        else if (op == "adiw") m.set_pair(reg_number(a_[0]),
                                          m.pair(reg_number(a_[0])) + static_cast<int>(literal(a_[1])));
        else if (op == "sbiw") {
            m.set_pair(reg_number(a_[0]),
                       m.pair(reg_number(a_[0])) - static_cast<int>(literal(a_[1])));
            if (reg_number(a_[0]) == 28 && m.frame < 0) m.frame = m.pair(28);
        }
        else if (op == "push") m.at(m.sp--) = m.reg[reg_number(a_[0])];
        else if (op == "pop") m.reg[reg_number(a_[0])] = m.at(++m.sp);
        else if (op == "lsl") m.reg[reg_number(a_[0])] = static_cast<uint8_t>(m.reg[reg_number(a_[0])] << 1);
        else if (op == "rol") m.reg[reg_number(a_[0])] = static_cast<uint8_t>(m.reg[reg_number(a_[0])] << 1);
        else if (op == "add" || op == "adc")
            m.reg[reg_number(a_[0])] = static_cast<uint8_t>(m.reg[reg_number(a_[0])] + m.reg[reg_number(a_[1])]);
        else if (op == "subi") m.reg[reg_number(a_[0])] = static_cast<uint8_t>(m.reg[reg_number(a_[0])] - literal(a_[1]));
        else if (op == "sbci") m.reg[reg_number(a_[0])] = static_cast<uint8_t>(m.reg[reg_number(a_[0])] - literal(a_[1]));
        else if (op == "sts") m.at(static_cast<int>(literal(a_[0]))) = m.reg[reg_number(a_[1])];
        else if (op == "lds") m.reg[reg_number(a_[0])] = m.at(static_cast<int>(literal(a_[1])));
        else if (op == "st" || op == "std") {
            int base = a_[0][0] == 'Y' ? m.pair(28) : m.pair(30);
            m.at(base + displacement(a_[0])) = m.reg[reg_number(a_[1])];
        } else if (op == "ld" || op == "ldd") {
            int base = a_[1][0] == 'Y' ? m.pair(28) : m.pair(30);
            m.reg[reg_number(a_[0])] = m.at(base + displacement(a_[1]));
        }
    }
}

// Runs a whole program's startup preamble -- everything before the entry point
// is called -- which is where globals are stored.
Machine run_globals(const std::string& assembly) {
    Machine m;
    run(m, assembly, "", "call setup");
    return m;
}

// Runs one function body. `sbiw r28, N` in the prologue moves Y down to the
// frame, so the frame pointer is whatever the prologue leaves behind.
Machine run_function(const std::string& assembly, const std::string& name) {
    Machine m;
    m.set_pair(28, 0x0800);
    run(m, assembly, name + ":", "ret");
    return m;
}

int word_at(Machine& m, int addr) { return m.at(addr) | (m.at(addr + 1) << 8); }

// Reads a 16-bit local out of the frame the interpreter just built. The
// generator starts locals at Y+1, because ldd/std reach Y+1..Y+63, and lays
// them out upwards in declaration order.
constexpr int kFirstLocal = 1;
int frame_base(Machine& m) { return (m.frame >= 0 ? m.frame : m.pair(28)) + kFirstLocal; }
int local_word(Machine& m, int offset) { return word_at(m, frame_base(m) + offset); }

// The first global always lands at the bottom of SRAM.
constexpr int kFirstGlobal = 0x0100;

bool assembles(const CompileResult& r) {
    if (!r.ok) return false;
    return ardio::assemble(r.assembly).ok;
}

} // namespace

// ---------------------------------------------------------------------------
// Dimension order. `int b[3][2]` is an array of 3 arrays of 2 ints: it must
// both print that way and index that way.
// ---------------------------------------------------------------------------

TEST(array_type_prints_its_outermost_dimension_first) {
    auto r = sketch("int b[3][2];\nint x = b;");
    CHECK(!r.ok);
    CHECK(r.error.find("int[3][2]") != std::string::npos);
}

TEST(array_type_prints_a_one_dimensional_array_the_same_way) {
    auto r = sketch("int a[4];\nint x = a;");
    CHECK(!r.ok);
    CHECK(r.error.find("int[4]") != std::string::npos);
}

TEST(two_dimensional_array_is_laid_out_row_major) {
    // Rows follow one another in memory, so b[1][0] is the third int.
    auto r = sketch("int b[3][2] = {{1, 2}, {3, 4}, {5, 6}};");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    for (int i = 0; i < 6; ++i) CHECK_EQ(word_at(m, kFirstGlobal + 2 * i), i + 1);
}

TEST(two_dimensional_array_reserves_every_element) {
    // 3 * 2 ints is 12 bytes, so the next global starts 12 bytes further on.
    auto r = sketch("int b[3][2] = {{1, 2}, {3, 4}, {5, 6}};\nint after = 0x4142;");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    CHECK_EQ(word_at(m, kFirstGlobal + 12), 0x4142);
}

TEST(two_dimensional_subscript_reaches_the_declared_element) {
    // b[2][1] is the sixth int. If nesting were built inside out this would
    // read some other element rather than failing to compile.
    auto r = build("int b[3][2] = {{1, 2}, {3, 4}, {5, 6}};\n"
                   "int picked;\n"
                   "int main() { picked = b[2][1]; return 0; }\n");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    run(m, r.assembly, "main:", "ret");
    CHECK_EQ(word_at(m, kFirstGlobal + 12), 6);
}

// ---------------------------------------------------------------------------
// Globals.
// ---------------------------------------------------------------------------

TEST(global_int_array_is_stored_before_the_entry_point) {
    auto r = sketch("int a[3] = {1, 2, 3};");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    CHECK_EQ(word_at(m, kFirstGlobal + 0), 1);
    CHECK_EQ(word_at(m, kFirstGlobal + 2), 2);
    CHECK_EQ(word_at(m, kFirstGlobal + 4), 3);
}

TEST(global_int_array_stores_negative_and_wide_values_little_endian) {
    auto r = sketch("int a[2] = {-1, 0x1234};");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    CHECK_EQ(int(m.at(kFirstGlobal + 0)), 0xFF);
    CHECK_EQ(int(m.at(kFirstGlobal + 1)), 0xFF);
    CHECK_EQ(int(m.at(kFirstGlobal + 2)), 0x34);
    CHECK_EQ(int(m.at(kFirstGlobal + 3)), 0x12);
}

TEST(global_char_array_from_a_string_literal_is_nul_terminated) {
    auto r = sketch("char s[] = \"AB\";");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    CHECK_EQ(int(m.at(kFirstGlobal + 0)), 'A');
    CHECK_EQ(int(m.at(kFirstGlobal + 1)), 'B');
    CHECK_EQ(int(m.at(kFirstGlobal + 2)), 0);
}

TEST(global_char_array_larger_than_its_string_is_zero_padded) {
    auto r = sketch("char s[6] = \"AB\";");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    CHECK_EQ(int(m.at(kFirstGlobal + 2)), 0);
    CHECK_EQ(int(m.at(kFirstGlobal + 5)), 0);
}

TEST(a_const_char_array_takes_a_string_literal_too) {
    auto r = sketch("const char s[] = \"hi\";");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    CHECK_EQ(int(m.at(kFirstGlobal + 0)), 'h');
    CHECK_EQ(int(m.at(kFirstGlobal + 2)), 0);
}

TEST(global_array_with_too_few_initialisers_is_zero_filled) {
    auto r = sketch("int a[4] = {7};");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    CHECK_EQ(word_at(m, kFirstGlobal + 0), 7);
    for (int i = 1; i < 4; ++i) CHECK_EQ(word_at(m, kFirstGlobal + 2 * i), 0);
}

TEST(a_short_row_of_a_two_dimensional_array_is_zero_filled) {
    auto r = sketch("int b[2][3] = {{1}, {4, 5}};");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    const int expected[6] = {1, 0, 0, 4, 5, 0};
    for (int i = 0; i < 6; ++i) CHECK_EQ(word_at(m, kFirstGlobal + 2 * i), expected[i]);
}

TEST(a_global_initialiser_may_be_a_constant_expression) {
    auto r = sketch("int base = 10;\nint a[2] = {base * 2, 1 << 4};");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    CHECK_EQ(word_at(m, kFirstGlobal + 2), 20);
    CHECK_EQ(word_at(m, kFirstGlobal + 4), 16);
}

TEST(a_non_constant_global_initialiser_is_still_rejected) {
    auto r = sketch("int f();\nint a[2] = {f(), 1};");
    CHECK(!r.ok);
    CHECK(r.error.find("not a constant") != std::string::npos);
}

TEST(global_arrays_assemble) {
    CHECK(assembles(sketch("int a[3] = {1, 2, 3};\nchar s[] = \"AB\";")));
}

// ---------------------------------------------------------------------------
// Locals.
// ---------------------------------------------------------------------------

TEST(local_int_array_is_filled_from_its_braces) {
    auto r = build("int main() { int c[3] = {7, 8, 9}; return 0; }");
    CHECK(r.ok);
    Machine m = run_function(r.assembly, "main");
    CHECK_EQ(local_word(m, 0), 7);
    CHECK_EQ(local_word(m, 2), 8);
    CHECK_EQ(local_word(m, 4), 9);
}

TEST(local_int_array_zero_fills_what_the_braces_left_out) {
    // SRAM is not zero at reset, so this has to be written, not assumed.
    auto r = build("int main() { int c[4] = {7}; return 0; }");
    CHECK(r.ok);
    Machine m;
    m.set_pair(28, 0x0800);
    for (int i = 0; i < 16; ++i) m.at(0x0800 - 16 + i) = 0xAA;   // poison the frame
    run(m, r.assembly, "main:", "ret");
    CHECK_EQ(local_word(m, 0), 7);
    CHECK_EQ(local_word(m, 2), 0);
    CHECK_EQ(local_word(m, 6), 0);
}

TEST(local_two_dimensional_array_is_row_major_too) {
    auto r = build("int main() { int b[3][2] = {{1, 2}, {3, 4}, {5, 6}}; return 0; }");
    CHECK(r.ok);
    Machine m = run_function(r.assembly, "main");
    for (int i = 0; i < 6; ++i) CHECK_EQ(local_word(m, 2 * i), i + 1);
}

TEST(local_char_array_from_a_string_literal) {
    auto r = build("int main() { char s[] = \"AB\"; return 0; }");
    CHECK(r.ok);
    Machine m = run_function(r.assembly, "main");
    int base = frame_base(m);
    CHECK_EQ(int(m.at(base + 0)), 'A');
    CHECK_EQ(int(m.at(base + 1)), 'B');
    CHECK_EQ(int(m.at(base + 2)), 0);
}

TEST(a_local_array_initialiser_can_name_earlier_locals) {
    auto r = build("int main() { int n = 5; int c[2] = {n, n + 1}; return 0; }");
    CHECK(r.ok);
    Machine m = run_function(r.assembly, "main");
    CHECK_EQ(local_word(m, 2), 5);
    CHECK_EQ(local_word(m, 4), 6);
}

TEST(a_local_declared_after_an_array_still_gets_its_own_slot) {
    // The stores spliced in after the declaration must not disturb the rest
    // of the block.
    auto r = build("int main() { int c[2] = {1, 2}; int tail = 0x4142; return 0; }");
    CHECK(r.ok);
    Machine m = run_function(r.assembly, "main");
    CHECK_EQ(local_word(m, 0), 1);
    CHECK_EQ(local_word(m, 2), 2);
    CHECK_EQ(local_word(m, 4), 0x4142);
}

TEST(local_arrays_assemble) {
    CHECK(assembles(build("int main() { int c[3] = {7, 8, 9}; char s[] = \"AB\"; return c[1]; }")));
}

TEST(a_setup_and_loop_sketch_may_use_a_local_array) {
    CHECK(assembles(build("void setup() { int c[3] = {1, 2, 3}; }\nvoid loop() {}\n")));
}

// ---------------------------------------------------------------------------
// Rejections.
// ---------------------------------------------------------------------------

TEST(too_many_initialisers_for_an_array_is_an_error) {
    auto r = build("int main() { int c[2] = {1, 2, 3}; return 0; }");
    CHECK(!r.ok);
    CHECK(r.error.find("initialiser") != std::string::npos);
}

TEST(too_many_initialisers_in_a_nested_row_is_an_error) {
    auto r = sketch("int b[2][2] = {{1, 2}, {3, 4, 5}};");
    CHECK(!r.ok);
}

TEST(a_string_literal_cannot_initialise_an_int_array) {
    auto r = sketch("int a[4] = \"AB\";");
    CHECK(!r.ok);
    CHECK(r.error.find("string literal") != std::string::npos);
}

TEST(a_string_literal_that_does_not_fit_is_an_error) {
    auto r = sketch("char s[2] = \"ABC\";");
    CHECK(!r.ok);
}

TEST(a_string_literal_exactly_as_long_as_the_array_drops_its_terminator) {
    // C allows this, and the Label Maker's alphabet relies on nothing more
    // than it: the array holds the characters and no NUL.
    auto r = sketch("char s[3] = \"ABC\";\nint after = 0x4142;");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    CHECK_EQ(int(m.at(kFirstGlobal + 0)), 'A');
    CHECK_EQ(int(m.at(kFirstGlobal + 2)), 'C');
    CHECK_EQ(word_at(m, kFirstGlobal + 3), 0x4142);
}

TEST(an_array_cannot_initialise_a_scalar) {
    auto r = sketch("int x = {1, 2};");
    CHECK(!r.ok);
}

TEST(a_braced_initialiser_for_a_scalar_holds_one_value) {
    auto r = sketch("int x = {42};");
    CHECK(r.ok);
    Machine m = run_globals(r.assembly);
    CHECK_EQ(word_at(m, kFirstGlobal), 42);
}

// ---------------------------------------------------------------------------
// Capacity. The Label Maker's font is int vector[63][14] -- 1764 bytes, which
// fits the ATmega328P's SRAM but leaves very little behind it.
// ---------------------------------------------------------------------------

namespace {

// Builds `int vector[rows][14] = {...}` with element (r, c) set to r * 14 + c,
// so any misplacement shows up as a wrong value rather than a wrong count.
std::string font_table(int rows) {
    std::string s = "int vector[" + std::to_string(rows) + "][14] = {\n";
    for (int r = 0; r < rows; ++r) {
        s += "{";
        for (int c = 0; c < 14; ++c) {
            if (c) s += ", ";
            s += std::to_string(r * 14 + c);
        }
        s += r + 1 == rows ? "}\n" : "},\n";
    }
    return s + "};\n";
}

} // namespace

TEST(the_label_maker_font_table_fits_in_sram) {
    auto r = sketch(font_table(63));
    CHECK(r.ok);
    if (!r.ok) return;
    Machine m = run_globals(r.assembly);
    // Spot-check the corners and a middle row, in row-major order.
    CHECK_EQ(word_at(m, kFirstGlobal + 0), 0);
    CHECK_EQ(word_at(m, kFirstGlobal + 2 * 13), 13);
    CHECK_EQ(word_at(m, kFirstGlobal + 2 * 14), 14);          // start of row 1
    CHECK_EQ(word_at(m, kFirstGlobal + 2 * (30 * 14 + 7)), 30 * 14 + 7);
    CHECK_EQ(word_at(m, kFirstGlobal + 2 * (62 * 14 + 13)), 62 * 14 + 13);
}

TEST(the_label_maker_font_table_assembles) {
    CHECK(assembles(sketch(font_table(63))));
}

TEST(a_table_too_large_for_sram_is_a_diagnostic_not_an_overflow) {
    auto r = sketch(font_table(100));       // 2800 bytes: no chip here has it
    CHECK(!r.ok);
    CHECK(r.error.find("SRAM") != std::string::npos);
    CHECK(r.error.find("2800") != std::string::npos);
}

TEST(the_sram_budget_is_reported_when_a_later_global_no_longer_fits) {
    auto r = sketch(font_table(63) + "int spare[64];\n");
    CHECK(!r.ok);
    CHECK(r.error.find("spare") != std::string::npos);
    CHECK(r.error.find("bytes of SRAM") != std::string::npos);
}
