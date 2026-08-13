#pragma once
#include "ardio/board.h"
#include "ardio/hex.h"
#include "ardio/platform/serial.h"
#include "ardio/protocol/programmer.h"

#include <cstdint>
#include <optional>
#include <vector>

// The Espressif ROM loader protocol, as spoken by the ESP8266's mask-ROM
// bootloader over the UART.
//
// Nothing here is AVR-like. Where STK500v1 sends bare command bytes terminated
// by CRC_EOP, the ROM loader sends SLIP-framed packets with a fixed header, and
// flash lives in an external SPI chip written in large blocks rather than in
// 128-byte on-die pages.
//
// Everything in this header is a pure function over bytes so the whole protocol
// can be checked byte-for-byte without hardware; the driver at the bottom is
// the only part that touches a SerialPort.
namespace ardio::esp_rom {

// ------------------------------------------------------------------ SLIP ---

inline constexpr uint8_t kSlipEnd    = 0xC0;  // frame delimiter
inline constexpr uint8_t kSlipEsc    = 0xDB;  // escape prefix
inline constexpr uint8_t kSlipEscEnd = 0xDC;  // 0xDB 0xDC means a literal 0xC0
inline constexpr uint8_t kSlipEscEsc = 0xDD;  // 0xDB 0xDD means a literal 0xDB

// Wraps `payload` in a SLIP frame: a leading 0xC0, the escaped body, a
// trailing 0xC0.
std::vector<uint8_t> slip_encode(const std::vector<uint8_t>& payload);

// Undoes slip_encode. `frame` must include both delimiters. Returns nullopt for
// a frame that is truncated, undelimited, or carries a bad escape pair.
std::optional<std::vector<uint8_t>> slip_decode(const std::vector<uint8_t>& frame);

// -------------------------------------------------------------- commands ---

enum class Command : uint8_t {
    FlashBegin = 0x02,
    FlashData  = 0x03,
    FlashEnd   = 0x04,
    Sync       = 0x08,
    ReadReg    = 0x0A,
};

inline constexpr uint8_t kDirRequest = 0x00;
inline constexpr uint8_t kDirReply   = 0x01;

// A request header is 8 bytes: direction, command, payload size (u16 LE), and a
// checksum field (u32 LE). The checksum field is only meaningful for
// FLASH_DATA; every other command leaves it zero.
inline constexpr size_t kHeaderSize = 8;

// The ROM's flash writes are done in 4096-byte blocks -- see Board::page_size
// for the ESP8266 entry, which carries the same number.
inline constexpr uint32_t kFlashBlockSize = 4096;

// Register holding the chip identification word, and the value an ESP8266
// answers with. Read with READ_REG, which returns the word in the reply's
// `value` field.
inline constexpr uint32_t kChipIdRegister = 0x3FF00010;
inline constexpr uint32_t kEsp8266ChipId  = 0xFFF0C101;

// XOR of every byte of a FLASH_DATA payload's *data* (not its header words),
// seeded with 0xEF. No other command's payload is checksummed.
uint8_t checksum(const uint8_t* data, size_t len);

// The unframed request: header followed by payload. Exposed separately from
// packet() so tests can read the header bytes directly.
std::vector<uint8_t> request_body(Command cmd, const std::vector<uint8_t>& payload,
                                  uint32_t checksum_field = 0);

// A complete, SLIP-framed request, ready to write to the port.
std::vector<uint8_t> packet(Command cmd, const std::vector<uint8_t>& payload,
                            uint32_t checksum_field = 0);

// SYNC's payload is a fixed magic sequence. The ROM answers a single SYNC
// several times over, so callers must drain the surplus replies.
std::vector<uint8_t> cmd_sync();

// Erases enough flash for `total_size` bytes at `offset` and puts the ROM into
// block-write mode. `blocks` is how many FLASH_DATA packets will follow.
std::vector<uint8_t> cmd_flash_begin(uint32_t total_size, uint32_t blocks,
                                     uint32_t block_size, uint32_t offset);

// One block of flash data. `sequence` counts from 0 and must not skip.
std::vector<uint8_t> cmd_flash_data(const uint8_t* data, size_t len, uint32_t sequence);

// Leaves block-write mode. `reboot` true runs the freshly written firmware;
// false keeps the ROM loader resident.
std::vector<uint8_t> cmd_flash_end(bool reboot);

std::vector<uint8_t> cmd_read_reg(uint32_t address);

// -------------------------------------------------------------- replies ----

struct Reply {
    uint8_t cmd = 0;
    uint32_t value = 0;       // READ_REG puts the register contents here
    std::vector<uint8_t> payload;
    bool status_ok = false;   // trailing status byte was 0
    uint8_t error = 0;        // ROM error code when status_ok is false
};

// Parses an already-SLIP-decoded reply body. Returns nullopt if the body is too
// short, is not marked as a reply, or declares a payload size that does not
// match the bytes present.
std::optional<Reply> parse_reply(const std::vector<uint8_t>& body);

// --------------------------------------------------------------- driver ----

// Resets the board into its ROM loader with GPIO0 held low, syncs, checks the
// chip ID, and writes `image` to flash in kFlashBlockSize blocks.
//
// UNVERIFIED AGAINST HARDWARE. Every byte this produces and every failure path
// is checked against a scripted ROM loader, but no physical ESP8266 has been
// flashed with it yet.
//
// UploadResult::stage names the phase that failed: "open", "sync", "chip",
// "size", "erase", "write", or "finish".
UploadResult upload_esp_rom(SerialPort& port, const std::string& path,
                            const Board& board, const HexImage& image,
                            const ProgressFn& progress);

// Drives the DTR/RTS reset dance on its own. Exposed for tests.
void enter_bootloader(SerialPort& port);

} // namespace ardio::esp_rom
