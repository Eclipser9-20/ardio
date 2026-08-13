#include "harness.h"
#include "ardio/cli.h"
#include <string>
#include <vector>

namespace {
ardio::Args parse(std::vector<const char*> argv) {
    return ardio::parse_args(int(argv.size()), const_cast<char**>(argv.data()));
}
} // namespace

TEST(cli_parses_bare_command) {
    auto a = parse({"ardio", "ports"});
    CHECK(a.command == "ports");
    CHECK(a.error.empty());
}

TEST(cli_parses_push_with_sketch_and_port) {
    auto a = parse({"ardio", "push", "blink.cpp", "--port", "/dev/cu.usbserial-110"});
    CHECK(a.command == "push");
    CHECK(a.positional == "blink.cpp");
    CHECK(a.port == "/dev/cu.usbserial-110");
}

TEST(cli_parses_board_and_baud) {
    auto a = parse({"ardio", "monitor", "--board", "nano", "--baud", "115200"});
    CHECK(a.board == "nano");
    CHECK_EQ(a.baud, 115200);
}

TEST(cli_parses_monitor_after_flag) {
    auto a = parse({"ardio", "push", "blink.cpp", "-m"});
    CHECK(a.monitor_after);
}

TEST(cli_errors_on_flag_missing_its_value) {
    auto a = parse({"ardio", "push", "--port"});
    CHECK(!a.error.empty());
    CHECK(a.error.find("--port") != std::string::npos);
}

TEST(cli_errors_on_unknown_flag) {
    auto a = parse({"ardio", "push", "--nonsense"});
    CHECK(a.error.find("--nonsense") != std::string::npos);
}

TEST(cli_no_command_requests_help) {
    auto a = parse({"ardio"});
    CHECK(a.help);
}

TEST(cli_help_flag_sets_help) {
    auto a = parse({"ardio", "--help"});
    CHECK(a.help);
}

// --- single-dash forms -------------------------------------------------

TEST(cli_accepts_single_dash_port) {
    auto a = parse({"ardio", "push", "-port", "/dev/ttyUSB0"});
    CHECK(a.port == "/dev/ttyUSB0");
    CHECK(a.error.empty());
    CHECK(!a.port_is_manual);
}

TEST(cli_accepts_single_dash_board_and_baud) {
    auto a = parse({"ardio", "monitor", "-board", "uno", "-baud", "9600"});
    CHECK(a.board == "uno");
    CHECK_EQ(a.baud, 9600);
    CHECK(a.error.empty());
}

TEST(cli_accepts_single_dash_monitor_and_backup) {
    auto a = parse({"ardio", "push", "blink.cpp", "-monitor", "-backup", "old.hex"});
    CHECK(a.monitor_after);
    CHECK(a.backup_first);
    CHECK(a.backup_path == "old.hex");
    CHECK(a.error.empty());
}

TEST(cli_accepts_single_dash_help) {
    auto a = parse({"ardio", "-help"});
    CHECK(a.help);
}

TEST(cli_double_dash_forms_still_work) {
    auto a = parse({"ardio", "push", "s.cpp", "--port", "/dev/a", "--board", "nano",
                    "--baud", "57600", "--monitor", "--backup"});
    CHECK(a.port == "/dev/a");
    CHECK(a.board == "nano");
    CHECK_EQ(a.baud, 57600);
    CHECK(a.monitor_after);
    CHECK(a.backup_first);
    CHECK(a.backup_path.empty());
    CHECK(a.error.empty());
}

// --- --manual ----------------------------------------------------------

TEST(cli_manual_sets_port_and_marks_it_manual) {
    auto a = parse({"ardio", "push", "-manual", "/dev/cu.usbmodem1"});
    CHECK(a.port == "/dev/cu.usbmodem1");
    CHECK(a.port_is_manual);
    CHECK(a.error.empty());
}

TEST(cli_manual_accepts_double_dash) {
    auto a = parse({"ardio", "push", "--manual", "/dev/cu.usbmodem1"});
    CHECK(a.port == "/dev/cu.usbmodem1");
    CHECK(a.port_is_manual);
}

TEST(cli_manual_missing_value_is_an_error) {
    auto a = parse({"ardio", "push", "-manual"});
    CHECK(!a.error.empty());
    CHECK(a.error.find("-manual") != std::string::npos);
}

// --- disambiguation ----------------------------------------------------

// The rule: an exact name match always beats a prefix match. "m" is an exact
// alias for monitor, so it must never resolve to "manual".
TEST(cli_dash_m_is_monitor_not_manual) {
    auto a = parse({"ardio", "push", "blink.cpp", "-m"});
    CHECK(a.monitor_after);
    CHECK(a.port.empty());
    CHECK(!a.port_is_manual);
    CHECK(a.error.empty());
}

TEST(cli_unique_prefix_resolves) {
    auto a = parse({"ardio", "push", "-man", "/dev/a"});
    CHECK(a.port == "/dev/a");
    CHECK(a.port_is_manual);
}

TEST(cli_ambiguous_prefix_is_an_error) {
    auto a = parse({"ardio", "push", "-ba", "x"});
    CHECK(a.error.find("-ba") != std::string::npos);
    CHECK(a.error.find("ambiguous") != std::string::npos);
}

TEST(cli_unknown_single_dash_flag_names_the_flag) {
    auto a = parse({"ardio", "push", "-nonsense"});
    CHECK(a.error.find("-nonsense") != std::string::npos);
}

TEST(cli_single_dash_flag_missing_its_value) {
    auto a = parse({"ardio", "push", "-board"});
    CHECK(!a.error.empty());
    CHECK(a.error.find("-board") != std::string::npos);
}
