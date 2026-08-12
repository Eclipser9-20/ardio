#include "harness.h"
#include "ardio/build.h"
#include "ardio/board.h"
#include <algorithm>
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
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    auto result = ardio::build_sketch("blink.cpp", *nano,
                                      {"/nonexistent-a", "/nonexistent-b"}, "/tmp");
    CHECK(!result.ok);
    CHECK(result.error.find("avr-g++") != std::string::npos);
    CHECK(result.error.find("/nonexistent-a") != std::string::npos);
    CHECK(result.error.find("/nonexistent-b") != std::string::npos);
}
