#include "harness.h"
#include "ardio/avr/compiler.h"
#include "ardio/avr/assembler.h"
#include <string>

namespace {

// Compiles, then assembles the result -- proving the generated text is not
// merely plausible but actually encodes to machine code.
bool compiles_and_assembles(const char* src, std::string& why) {
    auto c = ardio::compile_avr(src);
    if (!c.ok) { why = "compile: " + c.error; return false; }
    auto a = ardio::assemble(c.assembly);
    if (!a.ok) { why = "assemble: " + a.error + "\n--- assembly ---\n" + c.assembly; return false; }
    if (a.code.empty()) { why = "empty image"; return false; }
    return true;
}

} // namespace

TEST(compiler_rejects_a_program_with_no_entry_point) {
    auto r = ardio::compile_avr("int x;");
    CHECK(!r.ok);
    CHECK(r.error.find("entry point") != std::string::npos);
}

TEST(compiler_accepts_setup_and_loop) {
    std::string why;
    bool ok = compiles_and_assembles("void setup() { }\nvoid loop() { }\n", why);
    if (!ok) std::printf("    %s\n", why.c_str());
    CHECK(ok);
}

TEST(compiler_accepts_main) {
    std::string why;
    bool ok = compiles_and_assembles("int main() { return 0; }\n", why);
    if (!ok) std::printf("    %s\n", why.c_str());
    CHECK(ok);
}

TEST(compiler_emits_a_reset_sequence_before_the_entry_point) {
    auto r = ardio::compile_avr("void setup() { }\nvoid loop() { }\n");
    CHECK(r.ok);
    CHECK(r.assembly.find("clr  r1") != std::string::npos);
    CHECK(r.assembly.find("call setup") != std::string::npos);
    CHECK(r.assembly.find("call loop") != std::string::npos);
}

TEST(compiler_generates_locals_and_arithmetic) {
    std::string why;
    bool ok = compiles_and_assembles(
        "int main() {\n"
        "  int a = 2;\n"
        "  int b = 3;\n"
        "  int c = a + b * 2;\n"
        "  return c;\n"
        "}\n", why);
    if (!ok) std::printf("    %s\n", why.c_str());
    CHECK(ok);
}

TEST(compiler_generates_if_else) {
    std::string why;
    bool ok = compiles_and_assembles(
        "int main() {\n"
        "  int a = 1;\n"
        "  if (a > 0) { a = 10; } else { a = 20; }\n"
        "  return a;\n"
        "}\n", why);
    if (!ok) std::printf("    %s\n", why.c_str());
    CHECK(ok);
}

TEST(compiler_generates_while_with_break_and_continue) {
    std::string why;
    bool ok = compiles_and_assembles(
        "int main() {\n"
        "  int i = 0;\n"
        "  while (i < 10) {\n"
        "    i = i + 1;\n"
        "    if (i == 3) { continue; }\n"
        "    if (i == 8) { break; }\n"
        "  }\n"
        "  return i;\n"
        "}\n", why);
    if (!ok) std::printf("    %s\n", why.c_str());
    CHECK(ok);
}

TEST(compiler_generates_for_loops) {
    std::string why;
    bool ok = compiles_and_assembles(
        "int main() {\n"
        "  int total = 0;\n"
        "  for (int i = 0; i < 5; i = i + 1) { total = total + i; }\n"
        "  return total;\n"
        "}\n", why);
    if (!ok) std::printf("    %s\n", why.c_str());
    CHECK(ok);
}

TEST(compiler_generates_calls_with_parameters) {
    std::string why;
    bool ok = compiles_and_assembles(
        "int add(int a, int b) { return a + b; }\n"
        "int main() { return add(2, 3); }\n", why);
    if (!ok) std::printf("    %s\n", why.c_str());
    CHECK(ok);
}

TEST(compiler_spills_parameters_using_the_avr_abi) {
    // The first int argument arrives in r24:r25, the second in r22:r23.
    auto r = ardio::compile_avr("int add(int a, int b) { return a + b; }\n"
                                "int main() { return add(1, 2); }\n");
    CHECK(r.ok);
    CHECK(r.assembly.find("r24") != std::string::npos);
    CHECK(r.assembly.find("r22") != std::string::npos);
}

TEST(compiler_reports_a_parse_error_with_a_line_number) {
    auto r = ardio::compile_avr("int main() { return ; }\n");
    CHECK(!r.ok);
    CHECK(r.error.find("line") != std::string::npos);
}

TEST(compiler_generates_globals_in_sram) {
    std::string why;
    bool ok = compiles_and_assembles(
        "int counter = 7;\n"
        "void setup() { counter = 1; }\n"
        "void loop() { counter = counter + 1; }\n", why);
    if (!ok) std::printf("    %s\n", why.c_str());
    CHECK(ok);
}
