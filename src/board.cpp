#include "ardio/board.h"

namespace ardio {

const std::vector<Board>& board_database() {
    static const std::vector<Board> db = {
        Board{
            .id = "nano",
            .name = "Arduino Nano (ATmega328P)",
            .mcu = "atmega328p",
            .protocol = Protocol::Stk500v1,
            // 1A86:7523 CH340, 0403:6001 FTDI FT232R, 2341:0043 genuine Arduino
            .usb_ids = {{0x1A86, 0x7523}, {0x0403, 0x6001}, {0x2341, 0x0043}},
            // New bootloader first; old-bootloader clones fall back to 57600.
            .baud_rates = {115200, 57600},
            .flash_size = 32768,
            .page_size = 128,
            .signature = {0x1E, 0x95, 0x0F},
            .gcc_mcu = "atmega328p",
            .f_cpu = 16000000,
        },
    };
    return db;
}

const Board* find_board_by_id(std::string_view id) {
    for (const Board& b : board_database())
        if (b.id == id) return &b;
    return nullptr;
}

std::vector<const Board*> find_boards_by_usb(UsbId id) {
    std::vector<const Board*> out;
    for (const Board& b : board_database())
        for (const UsbId& u : b.usb_ids)
            if (u == id) { out.push_back(&b); break; }
    return out;
}

} // namespace ardio
