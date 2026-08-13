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
        Board{
            .id = "esp8266",
            .name = "ESP8266 (NodeMCU / ESP-12E / ESP-12F)",
            .mcu = "esp8266",
            .protocol = Protocol::EspRom,
            // 10C4:EA60 is the CP2102 bridge on NodeMCU v2 boards.
            //
            // These boards also ship with a CH340 (1A86:7523) -- but that pair
            // is the Nano's, and the two are genuinely indistinguishable over
            // USB: same bridge chip, no serial number, nothing downstream of it
            // is visible to the host. Claiming it here would make every CH340
            // port match two boards, and select_port() would (correctly) refuse
            // to guess -- which would break auto-detection for the Nano, the
            // common case, to serve the rarer one. So the CH340 stays with the
            // Nano and a CH340-based ESP8266 needs an explicit --board esp8266.
            .usb_ids = {{0x10C4, 0xEA60}},
            // The ROM loader autobauds, so one rate is enough.
            .baud_rates = {115200},
            // Flash is an external SPI chip, 4 MB on the usual modules, written
            // in 4096-byte blocks rather than in on-die pages.
            .flash_size = 4 * 1024 * 1024,
            .page_size = 4096,
            // No AVR-style signature bytes exist. Identity is a 32-bit word
            // read from a register instead -- see esp_rom::kEsp8266ChipId.
            .signature = {0x00, 0x00, 0x00},
            .gcc_mcu = "",     // not an avr-gcc target
            .f_cpu = 80000000,
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
