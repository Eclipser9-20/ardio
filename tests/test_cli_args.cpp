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
