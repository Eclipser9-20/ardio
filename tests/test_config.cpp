#include "harness.h"
#include "ardio/toolchain.h"
#include <string>

TEST(config_parses_string_and_int_values) {
    std::string err;
    auto cfg = ardio::parse_config(
        "default_board = \"nano\"\n"
        "monitor_baud = 9600\n", err);
    CHECK(err.empty());
    CHECK(cfg.default_board == "nano");
    CHECK_EQ(cfg.monitor_baud, 9600);
}

TEST(config_parses_search_root_array) {
    std::string err;
    auto cfg = ardio::parse_config(
        "search_roots = [\"/opt/mytools\", \"~/.ardio/tools\"]\n", err);
    CHECK(err.empty());
    CHECK_EQ(cfg.search_roots.size(), size_t(2));
    CHECK(cfg.search_roots[0] == "/opt/mytools");
    CHECK(cfg.search_roots[1] == "~/.ardio/tools");
}

TEST(config_ignores_comments_and_blank_lines) {
    std::string err;
    auto cfg = ardio::parse_config(
        "# a comment\n"
        "\n"
        "default_port = \"/dev/cu.usbserial-110\"   # trailing comment\n", err);
    CHECK(err.empty());
    CHECK(cfg.default_port == "/dev/cu.usbserial-110");
}

TEST(config_rejects_unknown_key) {
    std::string err;
    ardio::parse_config("nonsense = \"x\"\n", err);
    CHECK(err.find("nonsense") != std::string::npos);
    CHECK(err.find("line 1") != std::string::npos);
}

TEST(config_rejects_malformed_line) {
    std::string err;
    ardio::parse_config("default_board\n", err);
    CHECK(err.find("line 1") != std::string::npos);
}

TEST(config_defaults_are_sane_when_empty) {
    std::string err;
    auto cfg = ardio::parse_config("", err);
    CHECK(err.empty());
    CHECK_EQ(cfg.monitor_baud, 9600);
    CHECK(cfg.default_board.empty());
}

TEST(default_search_roots_are_ordered_ardio_first_path_last) {
    auto roots = ardio::default_search_roots();
    CHECK(roots.size() >= 4);
    CHECK(roots.front().find(".ardio") != std::string::npos);
    CHECK(roots.back() == "$PATH");
}

TEST(find_tool_reports_not_found_without_crashing) {
    auto loc = ardio::find_tool("definitely-not-a-real-tool-xyz",
                                {"/nonexistent-root-abc"});
    CHECK(!loc.found);
    CHECK(loc.path.empty());
}
