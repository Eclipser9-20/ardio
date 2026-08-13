#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ardio {

enum class Protocol { Stk500v1, EspRom };

struct UsbId {
    uint16_t vid = 0;
    uint16_t pid = 0;
    bool operator==(const UsbId&) const = default;
};

struct Board {
    std::string id;              // "nano"
    std::string name;            // "Arduino Nano (ATmega328P)"
    std::string mcu;             // "atmega328p"
    Protocol protocol = Protocol::Stk500v1;
    std::vector<UsbId> usb_ids;  // USB-serial bridges this board ships with
    std::vector<int> baud_rates; // bootloader baud, tried in order
    uint32_t flash_size = 0;
    uint32_t page_size = 0;
    std::array<uint8_t, 3> signature{};
    std::string gcc_mcu;         // value for avr-gcc -mmcu=
    int f_cpu = 0;
};

const std::vector<Board>& board_database();
const Board* find_board_by_id(std::string_view id);
std::vector<const Board*> find_boards_by_usb(UsbId id);

} // namespace ardio
