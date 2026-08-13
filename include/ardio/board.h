#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ardio {

enum class Protocol { Stk500v1, Stk500v2, Avr109, EspRom };

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

    // Ids that identify this board: seeing one is enough to name it. Only ids
    // burnt into a bridge the board actually owns belong here.
    std::vector<UsbId> usb_ids;

    // Ids the board presents only while its bootloader is running. On a 32U4
    // the USB device is the MCU itself, so it re-enumerates under a different
    // product id when it jumps to the bootloader; the sketch-mode id in
    // usb_ids is simply not on the bus at the moment we want to flash it.
    // These identify the board too -- they are kept separate because the
    // uploader needs to know which mode a port is in, not merely which board.
    std::vector<UsbId> bootloader_usb_ids;

    // Ids this board can appear as but which do NOT identify it, because some
    // other board presents the same id and nothing downstream of the bridge is
    // visible to the host. A generic CH340 or FT232R soldered to a clone is the
    // usual case. Recording them lets an ambiguous port produce a list of what
    // it might be instead of a shrug -- see boards_possible_for_usb().
    std::vector<UsbId> shared_usb_ids;

    std::vector<int> baud_rates; // bootloader baud, tried in order

    // Whether the board enters its bootloader by having the host open the port
    // at 1200 baud and drop DTR. The 32U4 boards need this; boards with a
    // hardware auto-reset circuit on DTR do not.
    bool touch_reset_1200 = false;

    uint32_t flash_size = 0;
    uint32_t page_size = 0;
    std::array<uint8_t, 3> signature{};
    std::string gcc_mcu;         // value for avr-gcc -mmcu=
    int f_cpu = 0;
};

const std::vector<Board>& board_database();
const Board* find_board_by_id(std::string_view id);

// Boards a USB id identifies, matching sketch-mode and bootloader-mode ids
// alike. More than one result means the host genuinely cannot tell which board
// is on the far end and the caller must ask the user rather than pick.
std::vector<const Board*> find_boards_by_usb(UsbId id);

// Everything the id could be, identifying matches plus boards that merely
// share the bridge. Never used to choose a board -- only to explain to the
// user what the choices are when they have to make it themselves.
std::vector<const Board*> boards_possible_for_usb(UsbId id);

} // namespace ardio
