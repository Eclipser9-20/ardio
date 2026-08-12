#include "harness.h"
#include "ardio/build.h"
#include "ardio/board.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

namespace {
bool has_arg(const ardio::CompileCommand& c, const std::string& want) {
    return std::find(c.args.begin(), c.args.end(), want) != c.args.end();
}
} // namespace

TEST(compile_command_sets_mcu_and_fcpu_from_board) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    auto cmd = ardio::make_compile_command("/opt/avr/bin/avr-g++", *nano,
                                           "blink.cpp", "blink.elf");
    CHECK(cmd.program == "/opt/avr/bin/avr-g++");
    CHECK(has_arg(cmd, "-mmcu=atmega328p"));
    CHECK(has_arg(cmd, "-DF_CPU=16000000L"));
}

TEST(compile_command_optimises_for_size_and_names_output) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    auto cmd = ardio::make_compile_command("/opt/avr/bin/avr-g++", *nano,
                                           "blink.cpp", "blink.elf");
    CHECK(has_arg(cmd, "-Os"));           // AVR flash is 32KB; size beats speed
    CHECK(has_arg(cmd, "blink.cpp"));
    CHECK(has_arg(cmd, "-o"));
    CHECK(has_arg(cmd, "blink.elf"));
}

TEST(objcopy_command_produces_ihex) {
    auto cmd = ardio::make_objcopy_command("/opt/avr/bin/avr-objcopy",
                                           "blink.elf", "blink.hex");
    CHECK(cmd.program == "/opt/avr/bin/avr-objcopy");
    CHECK(has_arg(cmd, "-O"));
    CHECK(has_arg(cmd, "ihex"));
    CHECK(has_arg(cmd, "-R"));
    CHECK(has_arg(cmd, ".eeprom"));       // EEPROM section is not flash content
    CHECK(has_arg(cmd, "blink.elf"));
    CHECK(has_arg(cmd, "blink.hex"));
}

TEST(build_reports_missing_toolchain_with_every_root_searched) {
    // A source ardio's own compiler cannot handle falls through to an external
    // toolchain; when that is absent too, the error must name every root.
    std::string path = std::string(ARDIO_TEST_TMP) + "/unsupported_source.cpp";
    {
        std::ofstream out(path);
        // The type system has no floating-point kind at all, so this is a
        // genuine limitation rather than a contrived one.
        out << "int main() { float f = 1.5; return 0; }\n";
    }

    const ardio::Board* nano = ardio::find_board_by_id("nano");
    auto result = ardio::build_sketch(path, *nano,
                                      {"/nonexistent-a", "/nonexistent-b"},
                                      ARDIO_TEST_TMP);
    std::remove(path.c_str());

    CHECK(!result.ok);
    CHECK(result.error.find("avr-g++") != std::string::npos);
    CHECK(result.error.find("/nonexistent-a") != std::string::npos);
    CHECK(result.error.find("/nonexistent-b") != std::string::npos);
    // The message must also say why ardio's own compiler declined, which is
    // the more useful half of the diagnosis.
    CHECK(result.error.find("could not build") != std::string::npos);
}

TEST(build_compiles_a_c_source_with_the_in_house_compiler) {
    // No external toolchain involved: ardio compiles, assembles and writes hex.
    std::string path = std::string(ARDIO_TEST_TMP) + "/tiny_program.cpp";
    {
        std::ofstream out(path);
        out << "int main() {\n"
               "  int total = 0;\n"
               "  for (int i = 0; i < 4; i = i + 1) { total = total + i; }\n"
               "  return total;\n"
               "}\n";
    }

    const ardio::Board* nano = ardio::find_board_by_id("nano");
    auto result = ardio::build_sketch(path, *nano, {"/nonexistent-root"}, ARDIO_TEST_TMP);
    std::remove(path.c_str());

    if (!result.ok) std::printf("    (build said: %s)\n", result.error.c_str());
    CHECK(result.ok);
    CHECK(result.hex_path.find(".hex") != std::string::npos);

    std::ifstream hex(result.hex_path);
    CHECK(hex.good());
    std::string first;
    std::getline(hex, first);
    CHECK(!first.empty());
    CHECK(first[0] == ':');          // a real Intel HEX record
}
