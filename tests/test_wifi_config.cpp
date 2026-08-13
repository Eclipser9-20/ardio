#include "harness.h"
#include "ardio/wifi_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace ardio;

namespace {

// Points the config at a scratch directory for the duration of a test, so a
// test can never read or overwrite the real one. Restored on destruction even
// if a check fails partway through.
class ScopedConfigHome {
public:
    explicit ScopedConfigHome(const std::string& dir) {
        const char* previous = std::getenv("XDG_CONFIG_HOME");
        had_ = previous != nullptr;
        if (had_) old_ = previous;
        ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    }
    ~ScopedConfigHome() {
        if (had_) ::setenv("XDG_CONFIG_HOME", old_.c_str(), 1);
        else ::unsetenv("XDG_CONFIG_HOME");
    }
private:
    bool had_ = false;
    std::string old_;
};

std::string scratch_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / ("ardio-wifi-test-" + std::string(name));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.string();
}

} // namespace

TEST(wifi_config_path_follows_xdg_config_home) {
    ScopedConfigHome home("/tmp/somewhere");
    CHECK(wifi_config_path() == "/tmp/somewhere/ardio/configuration.json");
}

TEST(loading_a_missing_config_is_empty_and_not_an_error) {
    ScopedConfigHome home(scratch_dir("missing"));
    WifiConfig config;
    std::string error;
    // Not having configured anything yet is the ordinary state, so this must
    // succeed rather than making every command report a failure on first run.
    CHECK(load_wifi_config(config, error));
    CHECK(error.empty());
    CHECK(config.boards.empty());
}

TEST(a_saved_board_round_trips) {
    ScopedConfigHome home(scratch_dir("roundtrip"));

    WifiConfig out;
    WifiBoard board;
    board.name = "pi-esp";
    board.board_id = "esp8266";
    board.host = "user@host.local";
    board.device = "/dev/serial0";
    board.ssid = "somenetwork";
    board.reset_gpio = 17;
    board.boot_gpio = 27;
    board.gpio_chip = "gpiochip4";
    out.boards.push_back(board);

    std::string error;
    CHECK(save_wifi_config(out, error));
    CHECK(error.empty());

    WifiConfig back;
    CHECK(load_wifi_config(back, error));
    CHECK_EQ(back.boards.size(), size_t(1));
    if (back.boards.size() == 1) {
        const WifiBoard& b = back.boards[0];
        CHECK(b.name == "pi-esp");
        CHECK(b.board_id == "esp8266");
        CHECK(b.host == "user@host.local");
        CHECK(b.device == "/dev/serial0");
        CHECK(b.ssid == "somenetwork");
        CHECK_EQ(b.reset_gpio, 17);
        CHECK_EQ(b.boot_gpio, 27);
        CHECK(b.gpio_chip == "gpiochip4");
    }
}

TEST(the_config_file_is_readable_only_by_its_owner) {
    ScopedConfigHome home(scratch_dir("perms"));

    WifiConfig out;
    WifiBoard board;
    board.name = "one";
    board.host = "user@host";
    out.boards.push_back(board);

    std::string error;
    CHECK(save_wifi_config(out, error));

    fs::perms p = fs::status(wifi_config_path()).permissions();
    // Nothing for group or other. This is not encryption, but a config file
    // describing how to reach someone's hardware should not be world readable.
    CHECK((p & fs::perms::group_all) == fs::perms::none);
    CHECK((p & fs::perms::others_all) == fs::perms::none);
}

TEST(fields_do_not_leak_between_neighbouring_boards) {
    ScopedConfigHome home(scratch_dir("two"));

    WifiConfig out;
    WifiBoard first;
    first.name = "first";
    first.host = "a@one";
    first.device = "/dev/ttyUSB0";
    first.reset_gpio = 5;
    WifiBoard second;
    second.name = "second";
    second.host = "b@two";
    second.device = "/dev/serial0";
    second.reset_gpio = 0;
    out.boards.push_back(first);
    out.boards.push_back(second);

    std::string error;
    CHECK(save_wifi_config(out, error));

    WifiConfig back;
    CHECK(load_wifi_config(back, error));
    CHECK_EQ(back.boards.size(), size_t(2));
    if (back.boards.size() == 2) {
        // The reader searches for keys by name, so the real risk is a record
        // picking up the next record's value. Pin it directly.
        CHECK(back.boards[0].host == "a@one");
        CHECK(back.boards[1].host == "b@two");
        CHECK(back.boards[0].device == "/dev/ttyUSB0");
        CHECK(back.boards[1].device == "/dev/serial0");
        CHECK_EQ(back.boards[0].reset_gpio, 5);
        CHECK_EQ(back.boards[1].reset_gpio, 0);
    }
}

TEST(a_quote_in_a_field_survives_the_round_trip) {
    ScopedConfigHome home(scratch_dir("escape"));

    WifiConfig out;
    WifiBoard board;
    board.name = "odd";
    board.host = "user@host";
    // A device path is user input, so it has to survive being written into
    // JSON and read back rather than terminating the string early.
    board.device = "/dev/a\"b\\c";
    out.boards.push_back(board);

    std::string error;
    CHECK(save_wifi_config(out, error));

    WifiConfig back;
    CHECK(load_wifi_config(back, error));
    CHECK_EQ(back.boards.size(), size_t(1));
    if (back.boards.size() == 1) CHECK(back.boards[0].device == "/dev/a\"b\\c");
}

TEST(find_wifi_board_matches_by_name_only) {
    WifiConfig config;
    WifiBoard board;
    board.name = "pi-esp";
    config.boards.push_back(board);

    CHECK(find_wifi_board(config, "pi-esp") != nullptr);
    CHECK(find_wifi_board(config, "pi") == nullptr);
    CHECK(find_wifi_board(config, "") == nullptr);
}
