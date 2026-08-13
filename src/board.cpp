#include "ardio/board.h"

#include <algorithm>

namespace ardio {

// A note on USB ids, because it governs most of what follows.
//
// Only two of these boards carry a USB id that belongs to them: the ones with
// an Atmel USB chip on board (the genuine Uno's ATmega16U2, the 32U4 boards'
// own MCU). Everything else reaches the host through a generic bridge -- a
// CH340, an FT232R -- that is the identical part on a clone Uno, a Nano, a
// Nano with the old bootloader, and the FTDI cable someone clipped to a Pro
// Mini. There is no serial number, no descriptor, nothing downstream of the
// bridge that the host can see. The boards are not merely hard to tell apart;
// they are indistinguishable.
//
// So generic bridge ids go in shared_usb_ids, which does not identify a board,
// with one deliberate exception: the CH340 and the FT232R stay in the Nano's
// usb_ids. That is a judgement call about the common case, not a claim of
// fact. Making those ids identify nothing would mean every CH340 Nano -- far
// and away the most-plugged-in board there is -- needed an explicit --board,
// to spare the rarer boards a prompt. The Nano's baud list covers the old
// bootloader as well, so the fallback is at worst slow, never wrong. What a
// wrong guess would cost is real, though: flashing a Pro Mini's 8 MHz timing
// into a 16 MHz Uno produces a sketch that runs at half speed and a serial
// port that emits garbage, with no error anywhere. Hence the shared_usb_ids
// list, so 'ardio' can at least say out loud what else the port might be.

const std::vector<Board>& board_database() {
    // Generic bridges, named once so the shared lists below read as what they
    // are rather than as a wall of hex.
    static const UsbId kCh340{0x1A86, 0x7523};
    static const UsbId kFt232r{0x0403, 0x6001};

    static const std::vector<Board> db = {
        Board{
            .id = "uno",
            .name = "Arduino Uno (ATmega328P)",
            .mcu = "atmega328p",
            .protocol = Protocol::Stk500v1,
            // Genuine Unos speak USB through an ATmega16U2 (0043 on the R3,
            // 0001 on the earlier 8U2 boards), which is a real identity: no
            // other board ships that firmware. It is still not unique here,
            // because Arduino's own board definitions list 2341:0043 for the
            // Nano too, so an official cable can land on either. That
            // ambiguity is left in rather than resolved arbitrarily -- both
            // boards really do claim it.
            .usb_ids = {{0x2341, 0x0043}, {0x2341, 0x0001}},
            .bootloader_usb_ids = {},
            // Clone Unos replace the 16U2 with a CH340 or an FTDI part and
            // become the Nano's twin over USB.
            .shared_usb_ids = {kCh340, kFt232r},
            .baud_rates = {115200},
            .touch_reset_1200 = false,
            .flash_size = 32768,
            .page_size = 128,
            .signature = {0x1E, 0x95, 0x0F},
            .gcc_mcu = "atmega328p",
            .f_cpu = 16000000,
        },
        Board{
            .id = "nano",
            .name = "Arduino Nano (ATmega328P)",
            .mcu = "atmega328p",
            .protocol = Protocol::Stk500v1,
            // 1A86:7523 CH340, 0403:6001 FTDI FT232R, 2341:0043 genuine Arduino
            .usb_ids = {kCh340, kFt232r, {0x2341, 0x0043}},
            .bootloader_usb_ids = {},
            .shared_usb_ids = {},
            // New bootloader first; old-bootloader clones fall back to 57600.
            .baud_rates = {115200, 57600},
            .touch_reset_1200 = false,
            .flash_size = 32768,
            .page_size = 128,
            .signature = {0x1E, 0x95, 0x0F},
            .gcc_mcu = "atmega328p",
            .f_cpu = 16000000,
        },
        Board{
            .id = "nano_old",
            .name = "Arduino Nano, old bootloader (ATmega328P)",
            .mcu = "atmega328p",
            .protocol = Protocol::Stk500v1,
            // This entry exists for one reason: the pre-2018 bootloader does
            // not answer at 115200 at all. It is not slower to sync, it is
            // silent, so the only cure is to talk to it at 57600 from the
            // start. Everything else about the board is a Nano.
            .usb_ids = {},
            .bootloader_usb_ids = {},
            .shared_usb_ids = {kCh340, kFt232r},
            .baud_rates = {57600},
            .touch_reset_1200 = false,
            .flash_size = 32768,
            .page_size = 128,
            .signature = {0x1E, 0x95, 0x0F},
            .gcc_mcu = "atmega328p",
            .f_cpu = 16000000,
        },
        Board{
            .id = "nano_168",
            .name = "Arduino Nano (ATmega168)",
            .mcu = "atmega168",
            .protocol = Protocol::Stk500v1,
            .usb_ids = {},
            .bootloader_usb_ids = {},
            .shared_usb_ids = {kCh340, kFt232r},
            // The ATmega168 Nano shipped with the 19200-baud bootloader.
            .baud_rates = {19200},
            .touch_reset_1200 = false,
            .flash_size = 16384,
            .page_size = 128,
            .signature = {0x1E, 0x94, 0x06},
            .gcc_mcu = "atmega168",
            .f_cpu = 16000000,
        },
        Board{
            .id = "pro_mini",
            .name = "Arduino Pro Mini 3.3V/8MHz (ATmega328P)",
            .mcu = "atmega328p",
            .protocol = Protocol::Stk500v1,
            // The Pro Mini has no USB hardware whatsoever -- it is programmed
            // through whatever FTDI or CH340 adapter is clipped to its header,
            // and that adapter's id says nothing about what is on the far end
            // of the six pins. So it identifies nothing and only ever appears
            // as a possibility.
            .usb_ids = {},
            .bootloader_usb_ids = {},
            .shared_usb_ids = {kCh340, kFt232r},
            .baud_rates = {57600},
            .touch_reset_1200 = false,
            .flash_size = 32768,
            .page_size = 128,
            .signature = {0x1E, 0x95, 0x0F},
            .gcc_mcu = "atmega328p",
            // The 3.3V Pro Mini runs its crystal at 8 MHz, not 16. Same chip,
            // same signature, same fuses as far as the programmer cares -- and
            // yet every delay, every baud divisor and every timer prescaler in
            // the compiled sketch is wrong if we assume 16. This is precisely
            // why f_cpu is a property of the board and not of the MCU: the
            // signature bytes cannot tell us, because the crystal is not part
            // of the chip.
            .f_cpu = 8000000,
        },
        Board{
            .id = "mega2560",
            .name = "Arduino Mega 2560 (ATmega2560)",
            .mcu = "atmega2560",
            .protocol = Protocol::Stk500v2,
            // 2341:0010 is the R3's 16U2, 2341:0042 the earlier revision.
            .usb_ids = {{0x2341, 0x0010}, {0x2341, 0x0042}},
            .bootloader_usb_ids = {},
            .shared_usb_ids = {kCh340, kFt232r},
            .baud_rates = {115200},
            .touch_reset_1200 = false,
            .flash_size = 262144,
            // 256 bytes, twice the 328P's page. Getting this wrong is not a
            // performance question: STK500v2 writes a page at a time and the
            // device commits whatever it was handed, so a short page leaves the
            // tail of each page holding the previous contents.
            .page_size = 256,
            .signature = {0x1E, 0x98, 0x01},
            .gcc_mcu = "atmega2560",
            .f_cpu = 16000000,
        },
        Board{
            .id = "mega1280",
            .name = "Arduino Mega 1280 (ATmega1280)",
            .mcu = "atmega1280",
            .protocol = Protocol::Stk500v2,
            // The 1280-era Mega predates the 16U2 and reached the host through
            // an FT232RL, so it has no id of its own to claim.
            .usb_ids = {},
            .bootloader_usb_ids = {},
            .shared_usb_ids = {kFt232r},
            // Its bootloader runs at 57600, unlike the 2560's.
            .baud_rates = {57600},
            .touch_reset_1200 = false,
            .flash_size = 131072,
            .page_size = 256,
            .signature = {0x1E, 0x97, 0x03},
            .gcc_mcu = "atmega1280",
            .f_cpu = 16000000,
        },
        Board{
            .id = "leonardo",
            .name = "Arduino Leonardo (ATmega32U4)",
            .mcu = "atmega32u4",
            .protocol = Protocol::Avr109,
            // The 32U4 is its own USB device: there is no bridge chip, the MCU
            // enumerates directly. That means the id on the bus depends on
            // which code is running. A Leonardo executing a sketch is
            // 2341:8036; when it resets into the bootloader it disappears from
            // the bus and comes back as 2341:0036, on a different serial port
            // node, for a few seconds only.
            //
            // Detection therefore has to accept both, and an uploader that
            // opened the sketch-mode port must expect that port to vanish under
            // it and go looking for the bootloader one. Treating these as a
            // single fixed id is the classic way to get "port disappeared"
            // halfway through a flash.
            .usb_ids = {{0x2341, 0x8036}},
            .bootloader_usb_ids = {{0x2341, 0x0036}},
            .shared_usb_ids = {},
            .baud_rates = {57600},
            // Nothing on a 32U4 board pulls reset from DTR. The bootloader is
            // entered by asking the running sketch to jump there, and the
            // agreed signal for that is a host opening the port at 1200 baud
            // and then dropping DTR.
            .touch_reset_1200 = true,
            .flash_size = 32768,
            .page_size = 128,
            .signature = {0x1E, 0x95, 0x87},
            .gcc_mcu = "atmega32u4",
            .f_cpu = 16000000,
        },
        Board{
            .id = "micro",
            .name = "Arduino Micro (ATmega32U4)",
            .mcu = "atmega32u4",
            .protocol = Protocol::Avr109,
            // Same chip and same two-mode behaviour as the Leonardo, one
            // product id along.
            .usb_ids = {{0x2341, 0x8037}},
            .bootloader_usb_ids = {{0x2341, 0x0037}},
            .shared_usb_ids = {},
            .baud_rates = {57600},
            .touch_reset_1200 = true,
            .flash_size = 32768,
            .page_size = 128,
            .signature = {0x1E, 0x95, 0x87},
            .gcc_mcu = "atmega32u4",
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
            .bootloader_usb_ids = {},
            .shared_usb_ids = {kCh340},
            // The ROM loader autobauds, so one rate is enough.
            .baud_rates = {115200},
            .touch_reset_1200 = false,
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

namespace {

bool contains(const std::vector<UsbId>& v, UsbId id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

} // namespace

std::vector<const Board*> find_boards_by_usb(UsbId id) {
    std::vector<const Board*> out;
    for (const Board& b : board_database())
        if (contains(b.usb_ids, id) || contains(b.bootloader_usb_ids, id))
            out.push_back(&b);
    return out;
}

std::vector<const Board*> boards_possible_for_usb(UsbId id) {
    std::vector<const Board*> out;
    for (const Board& b : board_database())
        if (contains(b.usb_ids, id) || contains(b.bootloader_usb_ids, id) ||
            contains(b.shared_usb_ids, id))
            out.push_back(&b);
    return out;
}

} // namespace ardio
