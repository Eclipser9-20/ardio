#include "ardio/protocol/programmer.h"
#include "ardio/protocol/stk500v1.h"

#include <chrono>
#include <thread>

namespace ardio {
namespace {

constexpr int kResponseTimeoutMs = 500;

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string hex3(const std::array<uint8_t, 3>& sig) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : sig) { s += d[b >> 4]; s += d[b & 0xF]; }
    return s;
}

// Sends a command and reads exactly `expected` response bytes.
std::vector<uint8_t> transact(SerialPort& port, const std::vector<uint8_t>& cmd,
                              size_t expected) {
    if (!port.write(cmd.data(), cmd.size())) return {};
    std::vector<uint8_t> resp(expected);
    size_t n = port.read(resp.data(), expected, kResponseTimeoutMs);
    resp.resize(n);
    return resp;
}

// Pulses DTR to yank RESET through the auto-reset capacitor, dropping the
// ATmega into its bootloader.
void pulse_reset(SerialPort& port) {
    port.set_dtr(true);
    port.set_rts(true);
    sleep_ms(50);
    port.set_dtr(false);
    port.set_rts(false);
    sleep_ms(50);
}

bool try_sync(SerialPort& port) {
    // The bootloader may have stale bytes buffered; a few attempts settles it.
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto resp = transact(port, stk500v1::cmd_get_sync(), 2);
        if (stk500v1::is_ok_response(resp)) return true;
        sleep_ms(50);
    }
    return false;
}

} // namespace

UploadResult upload_stk500v1(SerialPort& port, const std::string& path,
                             const Board& board, const HexImage& image,
                             const ProgressFn& progress) {
    UploadResult result;
    auto report = [&](const std::string& msg) { if (progress) progress(msg); };

    if (image.data.size() > board.flash_size) {
        result.stage = "size";
        result.error = "sketch is " + std::to_string(image.data.size()) +
                       " bytes but " + board.name + " has only " +
                       std::to_string(board.flash_size) + " bytes of flash";
        return result;
    }

    // Try each bootloader baud rate. Nano clones split between 115200 (new
    // bootloader) and 57600 (old); the user should never need to know which.
    std::string tried;
    bool synced = false;
    for (int baud : board.baud_rates) {
        if (!tried.empty()) tried += ", ";
        tried += std::to_string(baud);

        std::string open_error;
        if (!port.open(path, baud, open_error)) {
            result.stage = "open";
            result.error = open_error;
            return result;
        }
        report("trying " + std::to_string(baud) + " baud");
        pulse_reset(port);
        if (try_sync(port)) {
            synced = true;
            result.baud_used = baud;
            break;
        }
        port.close();
    }

    if (!synced) {
        result.stage = "sync";
        result.error = "no response from bootloader on " + path +
                       " at any known baud rate (tried " + tried +
                       "). Check the board is plugged in and not held in reset.";
        return result;
    }
    report("synced at " + std::to_string(result.baud_used) + " baud");

    if (!stk500v1::is_ok_response(transact(port, stk500v1::cmd_enter_progmode(), 2))) {
        result.stage = "sync";
        result.error = "bootloader refused to enter programming mode";
        return result;
    }

    // Signature response is: INSYNC, sig0, sig1, sig2, OK
    auto sig_resp = transact(port, stk500v1::cmd_read_signature(), 5);
    if (sig_resp.size() != 5 || sig_resp[0] != stk500v1::kInSync ||
        sig_resp[4] != stk500v1::kOk) {
        result.stage = "signature";
        result.error = "could not read device signature";
        return result;
    }
    std::array<uint8_t, 3> got{sig_resp[1], sig_resp[2], sig_resp[3]};
    if (got != board.signature) {
        result.stage = "signature";
        result.error = "device signature mismatch: expected " + hex3(board.signature) +
                       " for " + board.name + " but found " + hex3(got) +
                       ". Wrong --board, or a different chip than expected.";
        return result;
    }
    report("signature ok (" + hex3(got) + ")");

    // Write the image one flash page at a time.
    const uint32_t page = board.page_size;
    for (uint32_t offset = 0; offset < image.data.size(); offset += page) {
        uint32_t chunk = uint32_t(image.data.size()) - offset;
        if (chunk > page) chunk = page;

        uint16_t word_addr = uint16_t((image.base_address + offset) / 2);
        if (!stk500v1::is_ok_response(
                transact(port, stk500v1::cmd_load_address(word_addr), 2))) {
            result.stage = "write";
            result.error = "bootloader rejected load-address at byte offset " +
                           std::to_string(offset);
            return result;
        }
        if (!stk500v1::is_ok_response(
                transact(port,
                         stk500v1::cmd_prog_page(image.data.data() + offset, uint16_t(chunk)),
                         2))) {
            result.stage = "write";
            result.error = "page write failed at byte offset " + std::to_string(offset);
            return result;
        }
        report("wrote " + std::to_string(offset + chunk) + "/" +
               std::to_string(image.data.size()) + " bytes");
    }

    if (!stk500v1::is_ok_response(transact(port, stk500v1::cmd_leave_progmode(), 2))) {
        result.stage = "write";
        result.error = "bootloader refused to leave programming mode";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ardio
