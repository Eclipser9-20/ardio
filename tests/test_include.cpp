// #include and #define, end to end.
//
// compile_avr() has an overload that runs the real C preprocessor before the
// front end. These tests cover what a CrunchLabs-style sketch needs from it:
// pulling in ardio's own Arduino headers by name, using object-like macros as
// constants, and -- when something cannot be compiled -- getting a diagnostic
// that names the header instead of a line number pointing into expanded text.

#include "harness.h"
#include "ardio/avr/compiler.h"

#include <filesystem>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Tests run from the build directory as often as from the source root.
std::vector<std::string> runtime_include_paths() {
    for (const char* dir : {"runtime/include", "../runtime/include",
                            "../../runtime/include", "../../../runtime/include"}) {
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec)) return {std::string(dir)};
    }
    return {};
}

} // namespace

TEST(include_paths_are_found_from_the_test_working_directory) {
    CHECK(!runtime_include_paths().empty());
}

TEST(define_reaches_the_compiler_as_a_constant) {
    ardio::CompileResult r = ardio::compile_avr(
        "#define PEN_UP 90\n"
        "int a = PEN_UP;\n"
        "void setup() {}\n"
        "void loop() {}\n",
        {});
    CHECK(r.ok);
    CHECK(r.error.empty());
    // 90 stored into the global before the entry point runs.
    CHECK(r.assembly.find("ldi  r24, 90") != std::string::npos);
}

TEST(function_like_macro_expands_in_a_sketch) {
    ardio::CompileResult r = ardio::compile_avr(
        "#define DOUBLE(x) ((x) * 2)\n"
        "int a = DOUBLE(21);\n"
        "void setup() {}\n"
        "void loop() {}\n",
        {});
    CHECK(r.ok);
    CHECK(r.assembly.find("ldi  r24, 42") != std::string::npos);
}

TEST(conditionals_select_the_taken_branch) {
    ardio::CompileResult r = ardio::compile_avr(
        "#define FAST 1\n"
        "#if FAST\n"
        "int speed = 200;\n"
        "#else\n"
        "int speed = 5;\n"
        "#endif\n"
        "void setup() {}\n"
        "void loop() {}\n",
        {});
    CHECK(r.ok);
    CHECK(r.assembly.find("ldi  r24, 200") != std::string::npos);
    CHECK(r.assembly.find("ldi  r24, 5\n") == std::string::npos);
}

TEST(a_real_runtime_header_can_be_included_and_used) {
    std::vector<std::string> paths = runtime_include_paths();
    ardio::CompileResult r = ardio::compile_avr(
        "#include <Servo.h>\n"
        "#define PEN_UP 90\n"
        "Servo pen;\n"
        "void setup() { pen.attach(9); }\n"
        "void loop() { pen.write(PEN_UP); }\n",
        paths);
    CHECK(r.error.empty());
    CHECK(r.ok);
    // PEN_UP reached the code generator as the literal 90, passed to write().
    CHECK(r.assembly.find("ldi r24, 90") != std::string::npos);
    CHECK(r.assembly.find("call Servo__write") != std::string::npos);
}

TEST(the_label_makers_header_block_compiles) {
    std::vector<std::string> paths = runtime_include_paths();
    ardio::CompileResult r = ardio::compile_avr(
        "#include <Wire.h>\n"
        "#include <LiquidCrystal_I2C.h>\n"
        "#include <Stepper.h>\n"
        "#include <ezButton.h>\n"
        "#include <Servo.h>\n"
        "\n"
        "#define STEPS_PER_REV 2048\n"
        "#define PEN_UP 90\n"
        "#define PEN_DOWN 30\n"
        "\n"
        "int pen_position = PEN_UP;\n"
        "int steps = STEPS_PER_REV;\n"
        "void setup() {}\n"
        "void loop() {}\n",
        paths);
    CHECK(r.error.empty());
    CHECK(r.ok);
    CHECK(r.assembly.find("ldi  r24, 90") != std::string::npos);
}

TEST(a_header_included_twice_is_read_once) {
    std::vector<std::string> paths = runtime_include_paths();
    ardio::CompileResult r = ardio::compile_avr(
        "#include <Servo.h>\n"
        "#include <Servo.h>\n"
        "void setup() {}\n"
        "void loop() {}\n",
        paths);
    CHECK(r.error.empty());
    CHECK(r.ok);
}

TEST(a_missing_header_is_named_in_the_error) {
    std::vector<std::string> paths = runtime_include_paths();
    ardio::CompileResult r = ardio::compile_avr(
        "#include <Adafruit_NeoPixel.h>\n"
        "void setup() { strip.begin(); }\n"
        "void loop() {}\n",
        paths);
    CHECK(!r.ok);
    CHECK(r.error.find("Adafruit_NeoPixel.h") != std::string::npos);
    CHECK(r.error.find("cannot open") != std::string::npos);
    // And it says that blanking directives was tried and did not save it.
    CHECK(r.error.find("directives ignored") != std::string::npos);
}

TEST(a_missing_header_the_sketch_never_uses_still_builds) {
    std::vector<std::string> paths = runtime_include_paths();
    ardio::CompileResult r = ardio::compile_avr(
        "#include <Adafruit_NeoPixel.h>\n"
        "void setup() {}\n"
        "void loop() {}\n",
        paths);
    CHECK(r.ok);
}

// Including a header whose harder parts ardio cannot generate is fine as long
// as the sketch does not reach them: unreachable functions are not code
// generated, so a sketch pays only for what it calls. WString.h has fields
// ardio's generator cannot handle, and including it is still harmless.
TEST(a_header_with_ungeneratable_parts_is_fine_while_they_are_unused) {
    std::vector<std::string> paths = runtime_include_paths();
    ardio::CompileResult r = ardio::compile_avr(
        "#include <WString.h>\n"
        "void setup() {}\n"
        "void loop() {}\n",
        paths);
    if (!r.ok) std::printf("    (compiler said: %s)\n", r.error.c_str());
    CHECK(r.ok);
}

// The naming diagnostic still applies to a header that genuinely cannot be
// compiled at all -- a syntax error rather than an unreachable construct,
// since parsing happens whether or not anything calls the code.
TEST(a_header_ardio_cannot_compile_is_named_rather_than_a_line_number) {
    std::string dir = std::string(ARDIO_TEST_TMP) + "/broken_header_probe";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream out(dir + "/BrokenProbe.h");
        out << "#pragma once\nint = = ;\n";
    }

    ardio::CompileResult r = ardio::compile_avr(
        "#include <BrokenProbe.h>\n"
        "void setup() {}\n"
        "void loop() {}\n",
        std::vector<std::string>{dir});
    std::filesystem::remove_all(dir, ec);

    CHECK(!r.ok);
    CHECK(r.error.find("BrokenProbe.h") != std::string::npos);
}

TEST(an_error_in_the_sketch_itself_quotes_the_offending_line) {
    std::vector<std::string> paths = runtime_include_paths();
    ardio::CompileResult r = ardio::compile_avr(
        "#include <Servo.h>\n"
        "int a = nonsense;\n"
        "void setup() {}\n"
        "void loop() {}\n",
        paths);
    CHECK(!r.ok);
    CHECK(r.error.find("nonsense") != std::string::npos);
    CHECK(r.error.find("translation unit") != std::string::npos);
}

TEST(a_directive_free_source_keeps_its_original_line_numbers) {
    ardio::CompileResult r = ardio::compile_avr(
        "void setup() {}\n"
        "void loop() {}\n"
        "int a = nonsense;\n",
        {});
    CHECK(!r.ok);
    CHECK(r.error.find("line 3") != std::string::npos);
}
