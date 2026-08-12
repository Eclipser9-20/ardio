#pragma once
#include "ardio/board.h"
#include <string>
#include <string_view>
#include <vector>

namespace ardio {

struct PortInfo {
    std::string device;       // "/dev/cu.usbserial-110"
    std::string description;  // "USB Serial"
    UsbId usb;
    bool has_usb_id = false;
};

// Platform-specific. Lists candidate serial ports with USB IDs where known.
std::vector<PortInfo> enumerate_ports();

struct PortSelection {
    const PortInfo* port = nullptr;
    const Board* board = nullptr;
    std::string error;
};

// Applies auto-detection policy. Never guesses between candidates: if the
// choice is ambiguous it returns an error naming the options.
PortSelection select_port(const std::vector<PortInfo>& ports,
                          std::string_view wanted_port,
                          std::string_view wanted_board);

} // namespace ardio
