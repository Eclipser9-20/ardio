#pragma once
#include "ardio/board.h"
#include "ardio/hex.h"
#include "ardio/platform/serial.h"
#include <functional>
#include <string>

namespace ardio {

struct UploadResult {
    bool ok = false;
    std::string error;
    int baud_used = 0;
    // Which protocol phase failed: "open", "sync", "signature", "size",
    // "write", "verify". Empty on success.
    std::string stage;
};

using ProgressFn = std::function<void(const std::string&)>;

// Resets the board via DTR, syncs with the bootloader (trying each baud rate
// in board.baud_rates), verifies the device signature, and writes the image
// page by page. Pass nullptr for `progress` to report nothing.
UploadResult upload_stk500v1(SerialPort& port, const std::string& path,
                             const Board& board, const HexImage& image,
                             const ProgressFn& progress);

struct ReadResult {
    bool ok = false;
    std::string error;
    int baud_used = 0;
    std::string stage;              // as UploadResult::stage
    std::vector<uint8_t> data;      // flash contents, page by page
};

// Reads `byte_count` bytes of flash back off the board (0 means the whole
// flash), so firmware can be saved before being overwritten.
//
// UNVERIFIED AGAINST HARDWARE. The protocol layer is tested against a scripted
// bootloader and round-trips through the HEX writer, but a read from a real
// ATmega328P returned data that did not match the image just written to it.
// Do not rely on a dump as a backup until that is explained.
ReadResult read_flash_stk500v1(SerialPort& port, const std::string& path,
                               const Board& board, uint32_t byte_count,
                               const ProgressFn& progress);

} // namespace ardio
