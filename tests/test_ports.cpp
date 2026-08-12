#include "harness.h"
#include "ardio/platform/ports.h"
#include <string>

namespace {
ardio::PortInfo nano_port(const char* dev) {
    return ardio::PortInfo{dev, "USB Serial", {0x1A86, 0x7523}, true};
}
ardio::PortInfo unknown_port(const char* dev) {
    return ardio::PortInfo{dev, "Some Device", {0x9999, 0x0001}, true};
}
} // namespace

TEST(select_port_auto_picks_the_single_known_board) {
    std::vector<ardio::PortInfo> ports{nano_port("/dev/cu.usbserial-110")};
    auto sel = ardio::select_port(ports, "", "");
    CHECK(sel.port != nullptr);
    CHECK(sel.board != nullptr);
    CHECK(sel.board->id == "nano");
    CHECK(sel.error.empty());
}

TEST(select_port_refuses_to_guess_between_two_boards) {
    std::vector<ardio::PortInfo> ports{nano_port("/dev/cu.usbserial-110"),
                                       nano_port("/dev/cu.usbserial-210")};
    auto sel = ardio::select_port(ports, "", "");
    CHECK(sel.port == nullptr);
    CHECK(sel.error.find("--port") != std::string::npos);
    CHECK(sel.error.find("usbserial-110") != std::string::npos);
    CHECK(sel.error.find("usbserial-210") != std::string::npos);
}

TEST(select_port_explicit_port_wins_over_ambiguity) {
    std::vector<ardio::PortInfo> ports{nano_port("/dev/cu.usbserial-110"),
                                       nano_port("/dev/cu.usbserial-210")};
    auto sel = ardio::select_port(ports, "/dev/cu.usbserial-210", "");
    CHECK(sel.port != nullptr);
    CHECK(sel.port->device == "/dev/cu.usbserial-210");
}

TEST(select_port_errors_when_explicit_port_absent) {
    std::vector<ardio::PortInfo> ports{nano_port("/dev/cu.usbserial-110")};
    auto sel = ardio::select_port(ports, "/dev/cu.nope", "");
    CHECK(sel.port == nullptr);
    CHECK(sel.error.find("/dev/cu.nope") != std::string::npos);
    CHECK(sel.error.find("usbserial-110") != std::string::npos);  // lists what exists
}

TEST(select_port_requires_explicit_board_for_unknown_usb_id) {
    std::vector<ardio::PortInfo> ports{unknown_port("/dev/cu.usbserial-999")};
    auto sel = ardio::select_port(ports, "", "");
    CHECK(sel.board == nullptr);
    CHECK(sel.error.find("--board") != std::string::npos);
}

TEST(select_port_accepts_explicit_board_for_unknown_usb_id) {
    std::vector<ardio::PortInfo> ports{unknown_port("/dev/cu.usbserial-999")};
    auto sel = ardio::select_port(ports, "", "nano");
    CHECK(sel.port != nullptr);
    CHECK(sel.board != nullptr);
    CHECK(sel.board->id == "nano");
    CHECK(sel.error.empty());
}

TEST(select_port_rejects_unknown_board_name) {
    std::vector<ardio::PortInfo> ports{nano_port("/dev/cu.usbserial-110")};
    auto sel = ardio::select_port(ports, "", "teapot");
    CHECK(sel.board == nullptr);
    CHECK(sel.error.find("teapot") != std::string::npos);
}

TEST(select_port_reports_no_ports_found) {
    std::vector<ardio::PortInfo> ports;
    auto sel = ardio::select_port(ports, "", "");
    CHECK(sel.port == nullptr);
    CHECK(sel.error.find("no serial ports") != std::string::npos);
}
