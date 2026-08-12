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

} // namespace ardio
